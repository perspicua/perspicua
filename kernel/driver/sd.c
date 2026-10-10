/*
 * sd.c - Driver for the BCM2835 / Arasan SDHCI Controller.
 */

#include "driver/sd.h"
#include "driver/device.h"
#include "driver/gic.h"

#include "stdio.h"
#include "string.h"
#include "panic.h"
#include <stddef.h>
#include <stdint.h>

#include "uapi/errno.h"

#include "arch/irq.h"
#include "mm/addr.h"
#include "devicetree/fdt.h"
#include "core/timer.h"
#include "driver/block.h"
#include "driver/uart.h"
#include "driver/mailbox.h"
#include "core/lock.h"
#include "core/mutex.h"
#include "sched/sched.h"
#include "sched/wait.h"

/*
 * SD Operation Serializer
 * sd_op_mutex - Sleeping mutex providing exclusive access to the SD controller.
 *               No spinlock is held during transfers, allowing tasks to block.
 */
static struct kmutex sd_op_mutex = KMUTEX_INIT;

/*
 * Interrupt coordination between the ISR and the blocked task.
 *
 * sd_irq_lock    - Short irqsave spinlock protecting sd_irq_pending.
 * sd_irq_pending - Accumulated hardware interrupt bits (ISR reads+clears
 *                  the hardware W1C register and OR's into this word).
 * sd_irq_wq      - Wait queue for tasks waiting for SD controller interrupts.
 * sd_irq_num     - Cached IRQ line from the devicetree (0 = no IRQ).
 */
static spinlock_t sd_irq_lock = SPINLOCK_INIT;
static volatile uint32_t sd_irq_pending = 0;
static struct wait_queue sd_irq_wq = WAIT_QUEUE_INIT;
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

// Status Register Bits
#define STATUS_CMD_INHIBIT (1 << 0)
#define STATUS_DAT_INHIBIT (1 << 1)
#define STATUS_WRITE_READY (1 << 10)
#define STATUS_READ_READY  (1 << 11)
#define STATUS_CARD_INSERT (1 << 16)

// Interrupt Register Bits
#define INT_CMD_DONE   (1 << 0)
#define INT_DATA_DONE  (1 << 1)
#define INT_ERROR_MASK (0xFFFF0000)

// Command Register Bits (Upper 16 bits of xfer_mode_cmd)
#define CMD_RESP_NONE    (0 << 16)
#define CMD_RESP_136     (1 << 16)
#define CMD_RESP_48      (2 << 16)
#define CMD_RESP_48_BUSY (3 << 16)
#define CMD_CRC_CHECK_EN (1 << 19)
#define CMD_IDX_CHECK_EN (1 << 20)
#define CMD_HAS_DATA     (1 << 21)
#define CMD_IDX(i)       ((i & 0x3F) << 24)
#define CMD_TYPE_ABORT   (3 << 22)

// Transfer Mode Register Bits (Lower 16 bits of xfer_mode_cmd)
#define XFER_BLOCK_COUNT_EN (1 << 1)
#define XFER_AUTO_CMD12     (1 << 2)
#define XFER_READ           (1 << 4)
#define XFER_MULTI_BLOCK    (1 << 5)

// The controller counts the blocks down and sends the closing CMD12 itself.
#define XFER_MULTI (XFER_MULTI_BLOCK | XFER_BLOCK_COUNT_EN | XFER_AUTO_CMD12)

// SD Commands
#define CMD0  (CMD_IDX(0) | CMD_RESP_NONE)
#define CMD2  (CMD_IDX(2) | CMD_RESP_136 | CMD_CRC_CHECK_EN)
#define CMD3  (CMD_IDX(3) | CMD_RESP_48 | CMD_CRC_CHECK_EN)
#define CMD7  (CMD_IDX(7) | CMD_RESP_48_BUSY | CMD_CRC_CHECK_EN)
#define CMD8  (CMD_IDX(8) | CMD_RESP_48 | CMD_CRC_CHECK_EN | CMD_IDX_CHECK_EN)
#define CMD9  (CMD_IDX(9) | CMD_RESP_136 | CMD_CRC_CHECK_EN)
#define CMD12 (CMD_IDX(12) | CMD_RESP_48_BUSY | CMD_CRC_CHECK_EN | CMD_TYPE_ABORT)
#define CMD16 (CMD_IDX(16) | CMD_RESP_48 | CMD_CRC_CHECK_EN)
#define CMD17 (CMD_IDX(17) | CMD_RESP_48 | CMD_CRC_CHECK_EN | CMD_HAS_DATA | XFER_READ)
#define CMD18 (CMD_IDX(18) | CMD_RESP_48 | CMD_CRC_CHECK_EN | CMD_HAS_DATA | XFER_READ | XFER_MULTI)
#define CMD24 (CMD_IDX(24) | CMD_RESP_48 | CMD_CRC_CHECK_EN | CMD_HAS_DATA)
#define CMD25 (CMD_IDX(25) | CMD_RESP_48 | CMD_CRC_CHECK_EN | CMD_HAS_DATA | XFER_MULTI)
#define CMD55 (CMD_IDX(55) | CMD_RESP_48 | CMD_CRC_CHECK_EN)
#define ACMD6 (CMD_IDX(6) | CMD_RESP_48 | CMD_CRC_CHECK_EN)
#define ACMD41 (CMD_IDX(41) | CMD_RESP_48)

#define HOST_DATA_4BIT      (1 << 1)
#define CLK_INTERNAL_EN     (1 << 0)
#define CLK_INTERNAL_STABLE (1 << 1)
#define CLK_SD_EN           (1 << 2)
#define CLK_DIV_MASK        0xFFE0
#define CLK_RESET_CMD       (1 << 25)
#define CLK_RESET_DAT       (1 << 26)

// SDCLK = base / (2 * divider). The card must stay at or below 400 kHz until it is selected.
#define SD_ID_DIVIDER     0xFA
#define SD_DEFAULT_HZ     25000000
#define SD_IRQ_TIMEOUT_MS 1000
#define INT_STALE_MASK    (INT_CMD_DONE | INT_DATA_DONE | INT_ERROR_MASK)

// Blocks per command: under the 16-bit count register, and short enough to finish inside one timeout.
#define SD_MAX_BLOCKS 128

static sdhci_regs_t *regs = NULL;
static struct block_device sd_block_dev;
static uint32_t sd_rca = 0;
static int sd_is_sdhc = 0;

/*
 * sd_wait_status - Polls a hardware STATUS field, yielding between attempts.
 *
 * Called while holding sd_op_mutex; no spinlock is held, so sched_sleep_ms()
 * can safely call sched_schedule() without lockdep issues.
 */
static int sd_wait_status(uint32_t mask, uint32_t expected, int timeout_ms)
{
    // A card at full speed answers within microseconds; one sleep costs a whole scheduler tick.
    unsigned long spin_until = timer_get_system_time() + 2;
    while ((regs->status & mask) != expected) {
        if ((long)(timer_get_system_time() - spin_until) < 0) {
            asm volatile("yield");
            continue;
        }
        if (timeout_ms-- <= 0) {
            return -ETIMEDOUT;
        }
        sched_sleep_ms(1);
    }
    return 0;
}

// Atomically copies matching interrupt bits from sd_irq_pending to *bits and clears them.
static int sd_irq_take(uint32_t mask, uint32_t *bits)
{
    unsigned long flags = spin_lock_irqsave(&sd_irq_lock);
    if (sd_irq_pending & (mask | INT_ERROR_MASK)) {
        *bits = sd_irq_pending;
        sd_irq_pending &= ~(mask | INT_ERROR_MASK);
        spin_unlock_irqrestore(&sd_irq_lock, flags);
        return 1;
    }
    spin_unlock_irqrestore(&sd_irq_lock, flags);
    return 0;
}

/*
 * sd_wait_interrupt - Blocks until the SDHCI raises the requested interrupt.
 *
 * A transfer must not be abandoned halfway: the controller would finish it and
 * raise an unexpected interrupt during the next request.
 */
static int sd_wait_interrupt(uint32_t mask)
{
    struct task *cur = sched_current_task();
    if (!cur || !sd_irq_num) {
        // No IRQ or no scheduler context: poll instead of sleeping forever.
        int t = 1000;
        while (!(regs->interrupt & (mask | INT_ERROR_MASK)) && t--) {
            timer_sleep_ms(1);
        }
        uint32_t status = regs->interrupt;
        regs->interrupt = status & (mask | INT_ERROR_MASK);
        if (t < 0) {
            return -ETIMEDOUT;
        }
        if (status & INT_ERROR_MASK) {
            return -EIO;
        }
        return 0;
    }

    uint32_t bits = 0;
    int res = wq_wait_event_timeout(&sd_irq_wq, sd_irq_take(mask, &bits), SD_IRQ_TIMEOUT_MS);
    if (res != 0) {
        return res;
    }

    return (bits & INT_ERROR_MASK) ? -EIO : 0;
}

int sd_handle_irq(void)
{
    if (!regs) {
        return 0;
    }

    uint32_t bits = regs->interrupt;
    regs->interrupt = bits; // W1C: clear in hardware.

    unsigned long flags = spin_lock_irqsave(&sd_irq_lock);
    sd_irq_pending |= bits;
    spin_unlock_irqrestore(&sd_irq_lock, flags);

    return wq_wake_one(&sd_irq_wq);
}

unsigned int sd_get_irq(void)
{
    return sd_irq_num;
}

static int sd_send_cmd(uint32_t cmd, uint32_t arg)
{
    int res = sd_wait_status(STATUS_CMD_INHIBIT, 0, 100);
    if (res != 0) {
        return res;
    }

    if (cmd & CMD_HAS_DATA) {
        res = sd_wait_status(STATUS_DAT_INHIBIT, 0, 100);
        if (res != 0) {
            return res;
        }
    }

    // A late completion from an earlier, abandoned request must not satisfy this one.
    unsigned long flags = spin_lock_irqsave(&sd_irq_lock);
    regs->interrupt = INT_STALE_MASK;
    sd_irq_pending &= ~INT_STALE_MASK;
    spin_unlock_irqrestore(&sd_irq_lock, flags);

    regs->arg1 = arg;
    regs->xfer_mode_cmd = cmd;

    return sd_wait_interrupt(INT_CMD_DONE);
}

// Clears a command or data error so the next request starts from a clean controller.
static void sd_reset_lines(void)
{
    regs->clk_control |= CLK_RESET_CMD | CLK_RESET_DAT;
    for (int i = 0; i < 100 && (regs->clk_control & (CLK_RESET_CMD | CLK_RESET_DAT)); i++) {
        timer_sleep_ms(1);
    }
}

static int sd_set_divider(uint32_t div)
{
    regs->clk_control &= ~CLK_SD_EN;
    regs->clk_control = (regs->clk_control & ~CLK_DIV_MASK) | ((div & 0xFF) << 8)
                        | (((div >> 8) & 0x3) << 6) | CLK_INTERNAL_EN;
    for (int i = 0; !(regs->clk_control & CLK_INTERNAL_STABLE); i++) {
        if (i == 100) {
            return -ETIMEDOUT;
        }
        timer_sleep_ms(1);
    }
    regs->clk_control |= CLK_SD_EN;
    timer_sleep_ms(2);
    return 0;
}

// The firmware's current rate for one of its clocks, or 0 if it does not answer.
static uint32_t sd_get_clock(uint32_t id)
{
    unsigned int __attribute__((aligned(16))) mbox[8];
    mbox[0] = 8 * 4;
    mbox[1] = 0;
    mbox[2] = 0x00030002; // Get clock rate tag
    mbox[3] = 8;
    mbox[4] = 0;
    mbox[5] = id;
    mbox[6] = 0;
    mbox[7] = 0;

    mbox_call(mbox);
    return (mbox[1] == 0x80000000 && mbox[5] == id) ? mbox[6] : 0;
}

static int sd_set_clock(uint32_t clock)
{
    unsigned int __attribute__((aligned(16))) mbox[10];
    mbox[0] = 10 * 4;
    mbox[1] = 0;
    mbox[2] = 0x00038002; // Set clock rate tag
    mbox[3] = 12;
    mbox[4] = 8;
    mbox[5] = 1; // EMMC clock ID
    mbox[6] = clock;
    mbox[7] = 0;
    mbox[8] = 0;
    mbox[9] = 0;

    mbox_call(mbox);
    return (mbox[1] == 0x80000000) ? 0 : -EIO;
}

static int sd_init_host(void)
{
    // Reset the clock and wait for completion
    regs->clk_control |= (7 << 24);
    timer_sleep_ms(20);
    while (regs->clk_control & (7 << 24))
        ;

    regs->int_en = 0xFFFFFFFF;
    regs->int_mask = 0xFFFFFFFF;

    // Request 3.3V power
    regs->host_control = (regs->host_control & ~0xF00) | 0xE00;
    timer_sleep_ms(100);
    regs->host_control |= 0x100;

    // Enable internal clock
    regs->clk_control = (regs->clk_control & ~0xFFFF) | (0xFA << 8) | 0x01;
    while (!(regs->clk_control & 0x02))
        ;
    regs->clk_control |= 0x04;
    timer_sleep_ms(20);

    return 0;
}

/*
 * sd_csd_field - Extracts CSD[hi:lo] from a 136-bit R2 response.
 *
 * The controller strips the CRC byte, so the four RESP registers hold
 * CSD[127:8]: CSD bit n lives at bit (n - 8) of that 120-bit value, with
 * csd[0] holding the least significant word.
 */
static uint32_t sd_csd_field(const uint32_t csd[4], int hi, int lo)
{
    uint32_t out = 0;

    for (int bit = hi; bit >= lo; bit--) {
        int pos = bit - 8;
        out = (out << 1) | ((csd[pos / 32] >> (pos % 32)) & 1);
    }

    return out;
}

static int sd_init_card(void)
{
    if (sd_send_cmd(CMD0, 0) != 0) {
        return -EIO;
    }
    if (sd_send_cmd(CMD8, 0x1AA) != 0) {
        return -EIO;
    }

    int timeout = 1000;
    while (timeout--) {
        sd_send_cmd(CMD55, 0);
        sd_send_cmd(ACMD41, 0x40FF8000);
        if (regs->resp[0] & 0x80000000) {
            sd_is_sdhc = (regs->resp[0] & 0x40000000) ? 1 : 0;
            break;
        }
        timer_sleep_ms(1);
    }
    if (timeout <= 0) {
        return -ETIMEDOUT;
    }

    if (sd_send_cmd(CMD2, 0) != 0) {
        return -EIO;
    }
    if (sd_send_cmd(CMD3, 0) != 0) {
        return -EIO;
    }
    sd_rca = regs->resp[0] & 0xFFFF0000;

    if (sd_send_cmd(CMD9, sd_rca) == 0) {
        uint32_t csd[4] = {regs->resp[0], regs->resp[1], regs->resp[2], regs->resp[3]};

        /*
         * The capacity fields differ between CSD versions, and a card small
         * enough to still use v1.0 (as the QEMU-attached image does) decodes
         * to nonsense under the v2.0 formula. Dispatch on CSD_STRUCTURE.
         */
        if (sd_csd_field(csd, 127, 126) == 1) {
            // v2.0 (SDHC/SDXC): capacity is (C_SIZE + 1) * 512 KB.
            uint32_t c_size = sd_csd_field(csd, 69, 48);
            sd_block_dev.block_count = ((uint64_t)c_size + 1) * 1024;
        } else {
            /* v1.0 (SDSC): (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) blocks of
             * 2^READ_BL_LEN bytes, normalised to 512-byte blocks. */
            uint32_t c_size = sd_csd_field(csd, 73, 62);
            uint32_t c_mult = sd_csd_field(csd, 49, 47);
            uint32_t read_bl = sd_csd_field(csd, 83, 80);

            uint64_t bytes = ((uint64_t)c_size + 1) * (1ULL << (c_mult + 2)) * (1ULL << read_bl);
            sd_block_dev.block_count = bytes / 512;
        }
    }

    if (sd_send_cmd(CMD7, sd_rca) != 0) {
        return -EIO;
    }
    if (sd_send_cmd(CMD16, 512) != 0) {
        return -EIO;
    }

    return 0;
}

/*
 * sd_transfer - Moves count blocks starting at block through the data port.
 *
 * One block is CMD17 or CMD24; more are CMD18 or CMD25, which the controller
 * closes with CMD12 once the count runs out. in receives a read, out supplies
 * a write; exactly one of them is set.
 */
static int sd_transfer(size_t block, size_t count, uint32_t *in, const uint32_t *out)
{
    uint32_t addr = sd_is_sdhc ? (uint32_t)block : (uint32_t)block * 512;
    uint32_t cmd = count == 1 ? (in ? CMD17 : CMD24) : (in ? CMD18 : CMD25);
    uint32_t ready = in ? STATUS_READ_READY : STATUS_WRITE_READY;

    regs->blk_size_cnt = ((uint32_t)count << 16) | 512;
    int res = sd_send_cmd(cmd, addr);
    for (size_t i = 0; res == 0 && i < count; i++) {
        res = sd_wait_status(ready, ready, 500);
        if (res != 0) {
            break;
        }
        for (int j = 0; j < 128; j++) {
            if (in) {
                in[i * 128 + j] = regs->data;
            } else {
                regs->data = out[i * 128 + j];
            }
        }
    }
    if (res == 0) {
        res = sd_wait_interrupt(INT_DATA_DONE);
    }

    if (res != 0) {
        sd_reset_lines();
        // The controller does not close a transfer it gave up on; the card would wait in it forever.
        if (count > 1 && sd_send_cmd(CMD12, 0) != 0) {
            sd_reset_lines();
        }
    }
    return res;
}

static int sd_read_locked(uint32_t *buf, size_t start_block, size_t num_blocks)
{
    for (size_t done = 0; done < num_blocks;) {
        size_t n = num_blocks - done < SD_MAX_BLOCKS ? num_blocks - done : SD_MAX_BLOCKS;
        int res = sd_transfer(start_block + done, n, buf + done * 128, NULL);
        if (res != 0) {
            pr_err("sd: read of %lu blocks at %lu failed (%d)\n", n, start_block + done, res);
            return res;
        }
        done += n;
    }
    return 0;
}

static int sd_write_locked(const uint32_t *buf, size_t start_block, size_t num_blocks)
{
    for (size_t done = 0; done < num_blocks;) {
        size_t n = num_blocks - done < SD_MAX_BLOCKS ? num_blocks - done : SD_MAX_BLOCKS;
        int res = sd_transfer(start_block + done, n, NULL, buf + done * 128);
        if (res != 0) {
            pr_err("sd: write of %lu blocks at %lu failed (%d)\n", n, start_block + done, res);
            return res;
        }
        done += n;
    }
    return 0;
}

// Block 0 carries the 0x55AA signature whether it holds an MBR or a FAT boot sector.
static int sd_read_test(void)
{
    static uint32_t sector[128];
    if (sd_read_locked(sector, 0, 1) != 0) {
        return -EIO;
    }
    const uint8_t *bytes = (const uint8_t *)sector;
    return (bytes[510] == 0x55 && bytes[511] == 0xAA) ? 0 : -EIO;
}

/*
 * Moves a selected card from the 1-bit identification clock to a 4-bit bus at
 * up to 25 MHz. A failed test read restores the settings card init proved.
 */
static void sd_enable_fast_mode(uint32_t base_hz)
{
    if (base_hz == 0) {
        pr_warn("sd: base clock unknown; staying at the identification clock\n");
        return;
    }

    uint32_t div = (base_hz + 2 * SD_DEFAULT_HZ - 1) / (2 * SD_DEFAULT_HZ);
    if (div > 0x3FF) {
        div = 0x3FF;
    }

    if (sd_send_cmd(CMD55, sd_rca) != 0 || sd_send_cmd(ACMD6, 2) != 0) {
        pr_warn("sd: card refused a 4-bit bus; staying at the identification clock\n");
        return;
    }
    regs->host_control |= HOST_DATA_4BIT;

    if (sd_set_divider(div) == 0 && sd_read_test() == 0) {
        pr_info("sd: 4-bit bus at %u kHz\n", base_hz / (2 * div) / 1000);
        return;
    }

    sd_reset_lines();
    sd_set_divider(SD_ID_DIVIDER);
    regs->host_control &= ~HOST_DATA_4BIT;
    if (sd_send_cmd(CMD55, sd_rca) == 0) {
        sd_send_cmd(ACMD6, 0);
    }
    pr_warn("sd: fast mode failed its test read; staying at the identification clock\n");
}

int sd_read_blocks(struct block_device *dev, void *buffer, size_t start_block, size_t num_blocks)
{
    if (!dev->present) {
        return -ENOENT;
    }

    if (!buffer) {
        return -EINVAL;
    }

    /*
     * Reject out-of-range requests here: issuing CMD17 past the end of the
     * card leaves the controller in an error state that fails every later
     * transfer, so an isolated bad request would otherwise take down all
     * subsequent I/O.
     */
    if (start_block + num_blocks > dev->block_count || start_block + num_blocks < start_block) {
        return -EINVAL;
    }

    kmutex_lock(&sd_op_mutex);
    if (sd_op_mutex.depth > 1) {
        PANIC("sd: controller re-entered in the middle of a transfer");
    }
    int res = sd_read_locked((uint32_t *)buffer, start_block, num_blocks);
    kmutex_unlock(&sd_op_mutex);
    return res;
}

int sd_write_blocks(struct block_device *dev, const void *buffer, size_t start_block,
                    size_t num_blocks)
{
    if (!dev->present) {
        return -ENOENT;
    }

    if (!buffer) {
        return -EINVAL;
    }

    // See sd_read_blocks: an out-of-range command poisons the controller.
    if (start_block + num_blocks > dev->block_count || start_block + num_blocks < start_block) {
        return -EINVAL;
    }

    kmutex_lock(&sd_op_mutex);
    if (sd_op_mutex.depth > 1) {
        PANIC("sd: controller re-entered in the middle of a transfer");
    }
    int res = sd_write_locked((const uint32_t *)buffer, start_block, num_blocks);
    kmutex_unlock(&sd_op_mutex);
    return res;
}

static void sd_probe_abort(sdhci_regs_t *r)
{
    r->int_mask = 0;
    r->int_en = 0;
    r->interrupt = 0xFFFFFFFF;
    regs = NULL;

    /*
     * Release the IRQ so the next matching driver (bcm2711-emmc2) can claim
     * it. gic_disable_irq() must come first to ensure no stray interrupt
     * fires against the now-cleared handler table entry.
     */
    if (sd_irq_num) {
        gic_disable_irq(sd_irq_num);
        free_irq(sd_irq_num);
        sd_irq_num = 0;
    }
}

static irq_result_t sd_irq_handler(void *ctx)
{
    (void)ctx;
    return sd_handle_irq() ? IRQ_HANDLED_RESCHED : IRQ_HANDLED;
}

static int sd_probe(struct device *dev)
{
    /* We only support a single SD card. If one was already initialized, skip other matching
     * controllers. */
    if (sd_block_dev.present) {
        return -EEXIST;
    }

    uintptr_t vbase = devm_get_io_base(dev, 0);
    if (!vbase) {
        return -ENOENT;
    }

    sdhci_regs_t *r = (sdhci_regs_t *)vbase;
    if (!(r->status & STATUS_CARD_INSERT)) {
        return -ENOENT;
    }

    regs = r;

    /*
     * Enable the SDHCI interrupt in the GIC *before* sd_init_host/card()
     * so that sd_wait_interrupt() can block the boot task and be woken by
     * the ISR rather than busy-polling during card initialization.
     */
    sd_irq_num = devm_get_irq(dev, 0);
    if (sd_irq_num && request_irq(sd_irq_num, sd_irq_handler, NULL, "sd") != 0) {
        pr_warn("sd: IRQ %u already claimed; falling back to polling\n", sd_irq_num);
        sd_irq_num = 0;
    }

    if (sd_irq_num) {
        gic_enable_irq(sd_irq_num);
        pr_info("sd: interrupt-driven I/O enabled (IRQ %u)\n", sd_irq_num);
    } else {
        pr_warn("sd: no IRQ in devicetree, sd_wait_interrupt will poll\n");
    }

    if (sd_set_clock(100000000) < 0) {
        pr_err("sd: clock init failed\n");
        sd_probe_abort(r);
        return -EIO;
    }

    if (sd_init_host() != 0 || sd_init_card() != 0) {
        pr_err("sd: card init failed\n");
        sd_probe_abort(r);
        return -EIO;
    }

    // Firmware clock 12 feeds EMMC2, clock 1 the legacy controller.
    uint32_t base_hz = sd_get_clock(strcmp(dev->name, "bcm2711-emmc2") == 0 ? 12 : 1);
    if (base_hz == 0) {
        base_hz = ((regs->capabilities[0] >> 8) & 0xFF) * 1000000;
    }
    sd_enable_fast_mode(base_hz);

    sd_block_dev.block_size = 512;
    sd_block_dev.read_blocks = sd_read_blocks;
    sd_block_dev.write_blocks = sd_write_blocks;
    sd_block_dev.present = 1;
    strncpy(sd_block_dev.name, "sd0", sizeof(sd_block_dev.name));

    block_device_register(&sd_block_dev);

    size_t mb = (sd_block_dev.block_count * 512) / (1024 * 1024);
    pr_info("sd: %s card found: %lu MB (%lu blocks)\n", sd_is_sdhc ? "SDHC" : "SDSC", mb,
            sd_block_dev.block_count);
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
