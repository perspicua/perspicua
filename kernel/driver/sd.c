/*
 * sd.c - EMMC2 SD card driver for Raspberry Pi 4.
 *
 * This module provides block-level access via the SDHCI controller. It uses
 * PIO for data transfers and supports both hardware and QEMU environments.
 */

#include "driver/sd.h"

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

static int sd_wait_status(uint32_t mask, uint32_t expected, int timeout_ms)
{
    while (((regs->status & mask) != expected) && timeout_ms--) {
        sleep_ms(1);
    }
    return (timeout_ms >= 0) ? PERS_SUCCESS : -PERS_ERR_TIMED_OUT;
}

static int sd_wait_interrupt(uint32_t mask)
{
    int timeout_ms = 1000;
    while (!(regs->interrupt & (mask | INT_ERROR_MASK)) && timeout_ms--) {
        sleep_ms(1);
    }

    uint32_t status = regs->interrupt;
    /* Clear only the bits we were waiting for or error bits */
    regs->interrupt = status & (mask | INT_ERROR_MASK);

    if (timeout_ms < 0) {
        return -PERS_ERR_TIMED_OUT;
    }
    if (status & INT_ERROR_MASK) {
        return -PERS_ERR_IO_ERROR;
    }
    return PERS_SUCCESS;
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
    if (!dev->present) {
        return -PERS_ERR_NOT_FOUND;
    }
    uint32_t *buf = (uint32_t *)buffer;

    for (size_t i = 0; i < num_blocks; i++) {
        uint32_t addr = (uint32_t)(start_block + i);
        if (!sd_is_sdhc) {
            /* Standard capacity cards use byte-offsets */
            addr *= 512;
        }

        regs->blk_size_cnt = (1 << 16) | 512;
        int res = sd_send_cmd(CMD17, addr);
        if (res != PERS_SUCCESS) {
            pr_err("sd: read failed at block %lu\n", start_block + i);
            return res;
        }

        res = sd_wait_status(STATUS_READ_READY, STATUS_READ_READY, 500);
        if (res != PERS_SUCCESS) {
            return res;
        }

        /* PIO data transfer */
        for (int j = 0; j < 128; j++) {
            buf[i * 128 + j] = regs->data;
        }

        res = sd_wait_interrupt(INT_DATA_DONE);
        if (res != PERS_SUCCESS) {
            return res;
        }
    }

    return PERS_SUCCESS;
}

int sd_write_blocks(struct block_device *dev, const void *buffer, size_t start_block,
                    size_t num_blocks)
{
    if (!dev->present) {
        return -PERS_ERR_NOT_FOUND;
    }
    const uint32_t *buf = (const uint32_t *)buffer;

    for (size_t i = 0; i < num_blocks; i++) {
        regs->blk_size_cnt = (1 << 16) | 512;
        int res = sd_send_cmd(CMD24, (uint32_t)(start_block + i));
        if (res != PERS_SUCCESS) {
            return res;
        }

        res = sd_wait_status(STATUS_WRITE_READY, STATUS_WRITE_READY, 500);
        if (res != PERS_SUCCESS) {
            return res;
        }

        for (int j = 0; j < 128; j++) {
            regs->data = buf[i * 128 + j];
        }

        res = sd_wait_interrupt(INT_DATA_DONE);
        if (res != PERS_SUCCESS) {
            return res;
        }
    }

    return PERS_SUCCESS;
}

void sd_init(void)
{
    const char *compatibles[] = {"brcm,bcm2711-emmc2", "brcm,bcm2835-sdhci", NULL};
    const uint32_t *node = NULL;

    for (int i = 0; compatibles[i]; i++) {
        const uint32_t *n = fdt_find_node_by_compatible(compatibles[i]);
        if (!n) {
            continue;
        }

        struct fdt_property reg_prop;
        if (fdt_get_property(n, "reg", &reg_prop) != 0) {
            continue;
        }

        const uint32_t *reg_data = (const uint32_t *)reg_prop.value;
        uint32_t phys_base =
            (reg_prop.size >= 12) ? fdt32_to_cpu(reg_data[1]) : fdt32_to_cpu(reg_data[0]);

        if (phys_base < 0xFC000000) {
            phys_base = (phys_base & 0x01FFFFFF) | 0xFE000000;
        }

        sdhci_regs_t *r = (sdhci_regs_t *)P2V(phys_base);
        if (r->status & STATUS_CARD_INSERT) {
            node = n;
            regs = r;
            break;
        }
    }

    if (!node) {
        return;
    }

    if (sd_set_clock(100000000) < 0) {
        pr_err("sd: clock init failed\n");
        return;
    }

    if (sd_init_host() == PERS_SUCCESS && sd_init_card() == PERS_SUCCESS) {
        sd_block_dev.block_size = 512;
        sd_block_dev.read_blocks = sd_read_blocks;
        sd_block_dev.write_blocks = sd_write_blocks;
        sd_block_dev.present = 1;
        strncpy(sd_block_dev.name, "sd0", sizeof(sd_block_dev.name));

        block_device_register(&sd_block_dev);

        size_t mb = (sd_block_dev.block_count * 512) / (1024 * 1024);
        pr_info("sd: SDHC card found: %lu MB\n", mb);
    } else {
        pr_err("sd: card init failed\n");
    }
}
