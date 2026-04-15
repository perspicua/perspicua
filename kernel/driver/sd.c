/*
 * sd.c - EMMC2 SD card driver for Raspberry Pi 4.
 *
 * This module provides block-level access via the SDHCI controller. It uses
 * PIO for data transfers and supports both hardware and QEMU environments.
 */

#include "driver/sd.h"
#include "driver/device.h"
#include "driver/gic.h"

#include "stdio.h"
#include "string.h"
#include "types.h"

#include "uapi/errors.h"

#include "mm/addr.h"
#include "devicetree/fdt.h"
#include "core/timer.h"
#include "driver/block.h"
#include "driver/uart.h"
#include "driver/mailbox.h"
#include "core/lock.h"
#include "sched/sched.h"

/*
 * SD Operation Serializer
 * sd_op_lock  - irqsave spinlock guarding sd_op_busy and the wait queue
 * sd_op_busy  - set while one task owns the SD controller
 * sd_op_wq_*  - FIFO of tasks waiting for the controller (uses task->next)
 */
static spinlock_t sd_op_lock = SPINLOCK_INIT;
static int sd_op_busy = 0;
static struct task *sd_op_wq_head = NULL;
static struct task *sd_op_wq_tail = NULL;

/*
 * Interrupt coordination between the ISR and the blocked task.
 *
 * sd_irq_lock        - Short irqsave spinlock protecting the fields below.
 * sd_irq_pending     - Accumulated hardware interrupt bits (ISR reads+clears
 *                      the hardware W1C register and OR's into this word).
 * sd_waiting_task    - Task blocked in sd_wait_interrupt(), or NULL.
 * sd_irq_waiting_mask - Interrupt mask the task is waiting for.
 * sd_irq_num         - Cached IRQ line from the devicetree (0 = no IRQ).
 */
static spinlock_t sd_irq_lock = SPINLOCK_INIT;
static volatile uint32_t sd_irq_pending = 0;
static struct task *sd_waiting_task = NULL;
static uint32_t sd_irq_waiting_mask = 0;
static unsigned int sd_irq_num = 0;

typedef struct {
    volatile uint32_t arg2;
    volatile uint32_t blk_size_cnt;
    volatile uint32_t arg1;
    volatile uint32_t xfer_mode_cmd;
    volatile uint32_t resp[4];
    volatile uint32_t data;
    volatile uint32_t status;
    volatile uint32_t host_control;
    volatile uint32_t clk_control;
    volatile uint32_t interrupt;
    volatile uint32_t int_mask;
    volatile uint32_t int_en;
    volatile uint32_t host_control2;
    volatile uint32_t capabilities[2];
} sdhci_regs_t;

/* Status Register Bits */
#define STATUS_CMD_INHIBIT (1 << 0)
#define STATUS_DAT_INHIBIT (1 << 1)
#define STATUS_WRITE_READY (1 << 10)
#define STATUS_READ_READY  (1 << 11)
#define STATUS_CARD_INSERT (1 << 16)

/* Interrupt Register Bits */
#define INT_CMD_DONE   (1 << 0)
#define INT_DATA_DONE  (1 << 1)
#define INT_ERROR_MASK (0xFFFF0000)

/* Command Register Bits (Upper 16 bits of xfer_mode_cmd) */
#define CMD_RESP_NONE    (0 << 16)
#define CMD_RESP_136     (1 << 16)
#define CMD_RESP_48      (2 << 16)
#define CMD_RESP_48_BUSY (3 << 16)
#define CMD_CRC_CHECK_EN (1 << 19)
#define CMD_IDX_CHECK_EN (1 << 20)
#define CMD_HAS_DATA     (1 << 21)
#define CMD_IDX(i)       ((i & 0x3F) << 24)

/* Transfer Mode Register Bits (Lower 16 bits of xfer_mode_cmd) */
#define XFER_BLOCK_COUNT_EN (1 << 1)
#define XFER_READ           (1 << 4)
#define XFER_MULTI_BLOCK    (1 << 5)

/* SD Commands */
#define CMD0   (CMD_IDX(0) | CMD_RESP_NONE)
#define CMD2   (CMD_IDX(2) | CMD_RESP_136 | CMD_CRC_CHECK_EN)
#define CMD3   (CMD_IDX(3) | CMD_RESP_48 | CMD_CRC_CHECK_EN)
#define CMD7   (CMD_IDX(7) | CMD_RESP_48_BUSY | CMD_CRC_CHECK_EN)
#define CMD8   (CMD_IDX(8) | CMD_RESP_48 | CMD_CRC_CHECK_EN | CMD_IDX_CHECK_EN)
#define CMD9   (CMD_IDX(9) | CMD_RESP_136 | CMD_CRC_CHECK_EN)
#define CMD16  (CMD_IDX(16) | CMD_RESP_48 | CMD_CRC_CHECK_EN)
#define CMD17  (CMD_IDX(17) | CMD_RESP_48 | CMD_CRC_CHECK_EN | CMD_HAS_DATA | XFER_READ)
#define CMD24  (CMD_IDX(24) | CMD_RESP_48 | CMD_CRC_CHECK_EN | CMD_HAS_DATA)
#define CMD55  (CMD_IDX(55) | CMD_RESP_48 | CMD_CRC_CHECK_EN)
#define ACMD41 (CMD_IDX(41) | CMD_RESP_48)

static sdhci_regs_t *regs = NULL;
static struct block_device sd_block_dev;
static uint32_t sd_rca = 0;
static int sd_is_sdhc = 0;

/*
 * sd_op_acquire - Claim exclusive access to the SD controller.
 *
 * If another task is already running an SD operation the caller is queued and
 * blocked until that operation completes.  No lock is held on return, so
 * schedule() calls inside the operation do not pollute lockdep's per-core
 * held-lock tracking.
 */
static void sd_op_acquire(void)
{
    for (;;) {
        unsigned long flags = spin_lock_irqsave(&sd_op_lock);

        if (!sd_op_busy) {
            sd_op_busy = 1;
            spin_unlock_irqrestore(&sd_op_lock, flags);
            return;
        }

        struct task *cur = sched_get_current();
        if (!cur) {
            /* Early boot before scheduler: busy-wait (no tasks to switch to). */
            spin_unlock_irqrestore(&sd_op_lock, flags);
            while (sd_op_busy)
                ;
            continue;
        }

        /* Enqueue and block.  task->next is safe to use here because a
         * BLOCKED task is not in any scheduler queue. */
        cur->state = SCHED_TASK_BLOCKED;
        cur->next = NULL;
        if (sd_op_wq_tail) {
            sd_op_wq_tail->next = cur;
            sd_op_wq_tail = cur;
        } else {
            sd_op_wq_head = sd_op_wq_tail = cur;
        }
        spin_unlock_irqrestore(&sd_op_lock, flags);
        schedule(); /* No lock held — lockdep stays clean. */
    }
}

/*
 * sd_op_release - Relinquish exclusive access to the SD controller.
 *
 * Wakes the next queued waiter if one exists.
 */
static void sd_op_release(void)
{
    unsigned long flags = spin_lock_irqsave(&sd_op_lock);

    sd_op_busy = 0;

    struct task *t = sd_op_wq_head;
    if (t) {
        sd_op_wq_head = t->next;
        if (!sd_op_wq_head)
            sd_op_wq_tail = NULL;
        sched_unblock(t);
    }

    spin_unlock_irqrestore(&sd_op_lock, flags);
}

/*
 * sd_wait_status - Polls a hardware STATUS field, yielding between attempts.
 *
 * Called within an sd_op_acquire() / sd_op_release() region; no spinlock is
 * held, so sched_sleep_ms() can safely call schedule() without lockdep issues.
 */
static int sd_wait_status(uint32_t mask, uint32_t expected, int timeout_ms)
{
    while (((regs->status & mask) != expected) && timeout_ms--) {
        sched_sleep_ms(1);
    }
    return (timeout_ms >= 0) ? PERS_SUCCESS : -PERS_ERR_TIMED_OUT;
}

/*
 * sd_wait_interrupt - Blocks until the SDHCI raises the requested interrupt.
 *
 * Fast path: the ISR already set the bits in sd_irq_pending before we got
 * here — consume them and return without blocking.
 *
 * Slow path: mark the current task BLOCKED, release sd_irq_lock (re-enables
 * IRQs), and call schedule() with NO spinlock held so that lockdep sees a
 * clean lock state on both the sleeping and the woken task.
 *
 * Fallback: if sd_irq_num is 0 (no GIC binding) or there is no current task,
 * fall back to the original busy-wait polling so we never hang.
 */
static int sd_wait_interrupt(uint32_t mask)
{
    unsigned long flags = spin_lock_irqsave(&sd_irq_lock);

    /* Fast path: interrupt already collected by the ISR. */
    if (sd_irq_pending & (mask | INT_ERROR_MASK)) {
        uint32_t bits = sd_irq_pending;
        sd_irq_pending &= ~(mask | INT_ERROR_MASK);
        spin_unlock_irqrestore(&sd_irq_lock, flags);
        return (bits & INT_ERROR_MASK) ? -PERS_ERR_IO_ERROR : PERS_SUCCESS;
    }

    struct task *cur = sched_get_current();
    if (!cur || !sd_irq_num) {
        /* No IRQ or no scheduler context: poll instead of sleeping forever. */
        spin_unlock_irqrestore(&sd_irq_lock, flags);

        int t = 1000;
        while (!(regs->interrupt & (mask | INT_ERROR_MASK)) && t--)
            sleep_ms(1);
        uint32_t status = regs->interrupt;
        regs->interrupt = status & (mask | INT_ERROR_MASK);
        if (t < 0)
            return -PERS_ERR_TIMED_OUT;
        if (status & INT_ERROR_MASK)
            return -PERS_ERR_IO_ERROR;
        return PERS_SUCCESS;
    }

    /* Slow path: block.  No other spinlock is held at this point (sd_op
     * serialisation is done via the wait-queue, not a held spinlock). */
    sd_irq_waiting_mask = mask;
    sd_waiting_task = cur;
    cur->state = SCHED_TASK_BLOCKED;
    spin_unlock_irqrestore(&sd_irq_lock, flags); /* Re-enables IRQs. */

    schedule(); /* Woken by sd_handle_irq() → sched_unblock(). */

    /* Collect result posted by the ISR. */
    flags = spin_lock_irqsave(&sd_irq_lock);
    uint32_t bits = sd_irq_pending;
    sd_irq_pending &= ~(mask | INT_ERROR_MASK);
    sd_waiting_task = NULL;
    spin_unlock_irqrestore(&sd_irq_lock, flags);

    return (bits & INT_ERROR_MASK) ? -PERS_ERR_IO_ERROR : PERS_SUCCESS;
}

/*
 * sd_handle_irq - ISR body called from exception_irq_handler().
 *
 * Reads and clears the hardware interrupt register (W1C), accumulates the
 * bits, and unblocks any task that was waiting for those bits.
 *
 * Returns 1 if a task was unblocked (caller should call schedule()).
 */
int sd_handle_irq(void)
{
    if (!regs)
        return 0;

    uint32_t bits = regs->interrupt;
    regs->interrupt = bits; /* W1C: clear in hardware. */

    unsigned long flags = spin_lock_irqsave(&sd_irq_lock);
    sd_irq_pending |= bits;

    int woke = 0;
    if (sd_waiting_task && (sd_irq_pending & (sd_irq_waiting_mask | INT_ERROR_MASK))) {
        sched_unblock(sd_waiting_task);
        sd_waiting_task = NULL;
        woke = 1;
    }

    spin_unlock_irqrestore(&sd_irq_lock, flags);
    return woke;
}

/*
 * sd_get_irq - Returns the SDHCI IRQ line (0 = not probed yet).
 */
unsigned int sd_get_irq(void)
{
    return sd_irq_num;
}

static int sd_send_cmd(uint32_t cmd, uint32_t arg)
{
    int res = sd_wait_status(STATUS_CMD_INHIBIT, 0, 100);
    if (res != PERS_SUCCESS) {
        return res;
    }

    if (cmd & CMD_HAS_DATA) {
        res = sd_wait_status(STATUS_DAT_INHIBIT, 0, 100);
        if (res != PERS_SUCCESS) {
            return res;
        }
    }

    regs->arg1 = arg;
    regs->xfer_mode_cmd = cmd;

    return sd_wait_interrupt(INT_CMD_DONE);
}

static int sd_set_clock(uint32_t clock)
{
    unsigned int __attribute__((aligned(16))) mbox[10];
    mbox[0] = 10 * 4;
    mbox[1] = 0;
    mbox[2] = 0x00038002; /* Set clock rate tag */
    mbox[3] = 12;
    mbox[4] = 8;
    mbox[5] = 1; /* EMMC clock ID */
    mbox[6] = clock;
    mbox[7] = 0;
    mbox[8] = 0;
    mbox[9] = 0;

    mbox_call(mbox);
    return (mbox[1] == 0x80000000) ? PERS_SUCCESS : -PERS_ERR_IO_ERROR;
}

static int sd_init_host(void)
{
    /* Reset the clock and wait for completion */
    regs->clk_control |= (7 << 24);
    sleep_ms(20);
    while (regs->clk_control & (7 << 24))
        ;

    regs->int_en = 0xFFFFFFFF;
    regs->int_mask = 0xFFFFFFFF;

    /* Request 3.3V power */
    regs->host_control = (regs->host_control & ~0xF00) | 0xE00;
    sleep_ms(100);
    regs->host_control |= 0x100;

    /* Enable internal clock */
    regs->clk_control = (regs->clk_control & ~0xFFFF) | (0xFA << 8) | 0x01;
    while (!(regs->clk_control & 0x02))
        ;
    regs->clk_control |= 0x04;
    sleep_ms(20);

    return PERS_SUCCESS;
}

static int sd_init_card(void)
{
    if (sd_send_cmd(CMD0, 0) != PERS_SUCCESS) {
        return -PERS_ERR_IO_ERROR;
    }
    if (sd_send_cmd(CMD8, 0x1AA) != PERS_SUCCESS) {
        return -PERS_ERR_IO_ERROR;
    }

    int timeout = 1000;
    while (timeout--) {
        sd_send_cmd(CMD55, 0);
        sd_send_cmd(ACMD41, 0x40FF8000);
        if (regs->resp[0] & 0x80000000) {
            sd_is_sdhc = (regs->resp[0] & 0x40000000) ? 1 : 0;
            break;
        }
        sleep_ms(1);
    }
    if (timeout <= 0) {
        return -PERS_ERR_TIMED_OUT;
    }

    if (sd_send_cmd(CMD2, 0) != PERS_SUCCESS) {
        return -PERS_ERR_IO_ERROR;
    }
    if (sd_send_cmd(CMD3, 0) != PERS_SUCCESS) {
        return -PERS_ERR_IO_ERROR;
    }
    sd_rca = regs->resp[0] & 0xFFFF0000;

    if (sd_send_cmd(CMD9, sd_rca) == PERS_SUCCESS) {
        uint32_t c_size = ((regs->resp[2] & 0x3F) << 16) | (regs->resp[1] >> 16);
        sd_block_dev.block_count = (c_size + 1) * 1024;
    }

    if (sd_send_cmd(CMD7, sd_rca) != PERS_SUCCESS) {
        return -PERS_ERR_IO_ERROR;
    }
    if (sd_send_cmd(CMD16, 512) != PERS_SUCCESS) {
        return -PERS_ERR_IO_ERROR;
    }

    return PERS_SUCCESS;
}

int sd_read_blocks(struct block_device *dev, void *buffer, size_t start_block, size_t num_blocks)
{
    if (!dev->present)
        return -PERS_ERR_NOT_FOUND;

    uint32_t *buf = (uint32_t *)buffer;

    /*
     * sd_op_acquire() blocks until we have exclusive access but releases all
     * locks before returning, so the entire read runs with no spinlock held.
     * This keeps lockdep's per-core held-lock tracking clean across the
     * schedule() calls in sd_wait_interrupt() and sd_wait_status().
     */
    sd_op_acquire();

    for (size_t i = 0; i < num_blocks; i++) {
        uint32_t addr = (uint32_t)(start_block + i);
        if (!sd_is_sdhc)
            addr *= 512;

        regs->blk_size_cnt = (1 << 16) | 512;
        int res = sd_send_cmd(CMD17, addr);
        if (res != PERS_SUCCESS) {
            pr_err("sd: read failed at block %lu\n", start_block + i);
            sd_op_release();
            return res;
        }

        res = sd_wait_status(STATUS_READ_READY, STATUS_READ_READY, 500);
        if (res != PERS_SUCCESS) {
            sd_op_release();
            return res;
        }

        for (int j = 0; j < 128; j++)
            buf[i * 128 + j] = regs->data;

        res = sd_wait_interrupt(INT_DATA_DONE);
        if (res != PERS_SUCCESS) {
            sd_op_release();
            return res;
        }
    }

    sd_op_release();
    return PERS_SUCCESS;
}

int sd_write_blocks(struct block_device *dev, const void *buffer, size_t start_block,
                    size_t num_blocks)
{
    if (!dev->present)
        return -PERS_ERR_NOT_FOUND;

    const uint32_t *buf = (const uint32_t *)buffer;

    sd_op_acquire();

    for (size_t i = 0; i < num_blocks; i++) {
        uint32_t addr = (uint32_t)(start_block + i);
        if (!sd_is_sdhc) {
            addr *= 512;
        }

        regs->blk_size_cnt = (1 << 16) | 512;
        int res = sd_send_cmd(CMD24, addr);
        if (res != PERS_SUCCESS) {
            sd_op_release();
            return res;
        }

        res = sd_wait_status(STATUS_WRITE_READY, STATUS_WRITE_READY, 500);
        if (res != PERS_SUCCESS) {
            sd_op_release();
            return res;
        }

        for (int j = 0; j < 128; j++)
            regs->data = buf[i * 128 + j];

        res = sd_wait_interrupt(INT_DATA_DONE);
        if (res != PERS_SUCCESS) {
            sd_op_release();
            return res;
        }
    }

    sd_op_release();
    return PERS_SUCCESS;
}

static int sd_probe(struct device *dev)
{
    /* We only support a single SD card. If one was already initialized, skip other matching
     * controllers. */
    if (sd_block_dev.present) {
        return -PERS_ERR_ALREADY_EXISTS;
    }

    uintptr_t vbase = devm_get_io_base(dev, 0);
    if (!vbase) {
        return -PERS_ERR_NOT_FOUND;
    }

    sdhci_regs_t *r = (sdhci_regs_t *)vbase;
    if (!(r->status & STATUS_CARD_INSERT)) {
        return -PERS_ERR_NOT_FOUND;
    }

    regs = r;

    /*
     * Enable the SDHCI interrupt in the GIC *before* sd_init_host/card()
     * so that sd_wait_interrupt() can block the boot task and be woken by
     * the ISR rather than busy-polling during card initialization.
     */
    sd_irq_num = devm_get_irq(dev, 0);
    if (sd_irq_num) {
        gic_enable_irq(sd_irq_num);
        pr_info("sd: interrupt-driven I/O enabled (IRQ %u)\n", sd_irq_num);
    } else {
        pr_warn("sd: no IRQ in devicetree, sd_wait_interrupt will poll\n");
    }

    if (sd_set_clock(100000000) < 0) {
        pr_err("sd: clock init failed\n");
        return -PERS_ERR_IO_ERROR;
    }

    if (sd_init_host() != PERS_SUCCESS || sd_init_card() != PERS_SUCCESS) {
        pr_err("sd: card init failed\n");
        return -PERS_ERR_IO_ERROR;
    }

    sd_block_dev.block_size = 512;
    sd_block_dev.read_blocks = sd_read_blocks;
    sd_block_dev.write_blocks = sd_write_blocks;
    sd_block_dev.present = 1;
    strncpy(sd_block_dev.name, "sd0", sizeof(sd_block_dev.name));

    block_device_register(&sd_block_dev);

    size_t mb = (sd_block_dev.block_count * 512) / (1024 * 1024);
    pr_info("sd: SDHC card found: %lu MB\n", mb);
    return 0;
}

DEVICE_DRIVER(bcm2711_emmc2) = {
    .name = "bcm2711-emmc2",
    .compatible = "brcm,bcm2711-emmc2",
    .probe = sd_probe,
};

DEVICE_DRIVER(bcm2835_sdhci) = {
    .name = "bcm2835-sdhci",
    .compatible = "brcm,bcm2835-sdhci",
    .probe = sd_probe,
};
