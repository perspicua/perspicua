/*
 * sd.c - EMMC2 SD card driver for Raspberry Pi 4
 *
 * This driver provides block-level access to the SD card via the SDHCI
 * controller. It uses PIO for data transfers and supports both hardware
 * and QEMU-emulated environments.
 */

#include "driver/sd.h"
#include "driver/block.h"
#include "driver/uart.h"
#include "driver/mailbox.h"
#include "timer.h"
#include "types.h"
#include "stdio.h"
#include "addr.h"
#include "string.h"
#include "devicetree/pht.h"
#include "uapi/errors.h"

typedef struct
{
    volatile uint32_t arg2;             // 0x00: Argument 2
    volatile uint32_t blk_size_cnt;     // 0x04: Block Size & Block Count
    volatile uint32_t arg1;             // 0x08: Argument 1
    volatile uint32_t xfer_mode_cmd;    // 0x0C: Transfer Mode & Command
    volatile uint32_t resp[4];          // 0x10: Response 0-3
    volatile uint32_t data;             // 0x20: Data Port
    volatile uint32_t status;           // 0x24: Present State
    volatile uint32_t host_control;     // 0x28: Host/Power/Block Gap/Wakeup Control
    volatile uint32_t clk_control;      // 0x2C: Clock/Timeout/Software Reset Control
    volatile uint32_t interrupt;        // 0x30: Interrupt Status
    volatile uint32_t int_mask;         // 0x34: Interrupt Status Enable
    volatile uint32_t int_en;           // 0x38: Interrupt Signal Enable
    volatile uint32_t host_control2;    // 0x3C: Host Control 2
    volatile uint32_t capabilities[2];  // 0x40: Capabilities 0-1
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
#define CMD0   CMD_IDX(0) | CMD_RESP_NONE
#define CMD2   CMD_IDX(2) | CMD_RESP_136 | CMD_CRC_CHECK_EN
#define CMD3   CMD_IDX(3) | CMD_RESP_48 | CMD_CRC_CHECK_EN
#define CMD7   CMD_IDX(7) | CMD_RESP_48_BUSY | CMD_CRC_CHECK_EN
#define CMD8   CMD_IDX(8) | CMD_RESP_48 | CMD_CRC_CHECK_EN | CMD_IDX_CHECK_EN
#define CMD9   CMD_IDX(9) | CMD_RESP_136 | CMD_CRC_CHECK_EN
#define CMD16  CMD_IDX(16) | CMD_RESP_48 | CMD_CRC_CHECK_EN
#define CMD17  CMD_IDX(17) | CMD_RESP_48 | CMD_CRC_CHECK_EN | CMD_HAS_DATA | XFER_READ
#define CMD24  CMD_IDX(24) | CMD_RESP_48 | CMD_CRC_CHECK_EN | CMD_HAS_DATA
#define CMD55  CMD_IDX(55) | CMD_RESP_48 | CMD_CRC_CHECK_EN
#define ACMD41 CMD_IDX(41) | CMD_RESP_48

static sdhci_regs_t* regs = NULL;
static struct block_device sd_block_dev;
static uint32_t sd_rca = 0;

static int sd_wait_status(uint32_t mask, uint32_t expected, int timeout_ms)
{
    while (((regs->status & mask) != expected) && timeout_ms--)
    {
        sleep_ms(1);
    }
    return timeout_ms >= 0 ? PERS_SUCCESS : -PERS_ERR_TIMED_OUT;
}

static int sd_wait_interrupt(uint32_t mask)
{
    int timeout_ms = 1000;
    while (!(regs->interrupt & (mask | INT_ERROR_MASK)) && timeout_ms--)
    {
        sleep_ms(1);
    }

    uint32_t status = regs->interrupt;
    regs->interrupt = status;  // Acknowledge

    if (timeout_ms < 0)
    {
        return -PERS_ERR_TIMED_OUT;
    }
    if (status & INT_ERROR_MASK)
    {
        return -PERS_ERR_IO_ERROR;
    }
    return PERS_SUCCESS;
}

static int sd_send_cmd(uint32_t cmd, uint32_t arg)
{
    int res = sd_wait_status(STATUS_CMD_INHIBIT, 0, 100);
    if (res != PERS_SUCCESS)
    {
        return res;
    }

    regs->arg1 = arg;
    regs->xfer_mode_cmd = cmd;

    return sd_wait_interrupt(INT_CMD_DONE);
}

/* --- Block Device Interface --- */

int sd_read_blocks(struct block_device* dev, void* buffer, size_t start_block, size_t num_blocks)
{
    if (!dev->present)
        return -PERS_ERR_NOT_FOUND;
    uint32_t* buf = (uint32_t*)buffer;

    for (size_t i = 0; i < num_blocks; i++)
    {
        regs->blk_size_cnt = (1 << 16) | 512;  // 1 block of 512 bytes
        int res = sd_send_cmd(CMD17, (uint32_t)(start_block + i));
        if (res != PERS_SUCCESS)
            return res;

        res = sd_wait_status(STATUS_READ_READY, STATUS_READ_READY, 500);
        if (res != PERS_SUCCESS)
            return res;

        for (int j = 0; j < 128; j++)  // 512 / 4
        {
            buf[i * 128 + j] = regs->data;
        }

        res = sd_wait_interrupt(INT_DATA_DONE);
        if (res != PERS_SUCCESS)
            return res;
    }

    return PERS_SUCCESS;
}

int sd_write_blocks(struct block_device* dev, const void* buffer, size_t start_block, size_t num_blocks)
{
    if (!dev->present)
        return -PERS_ERR_NOT_FOUND;
    const uint32_t* buf = (const uint32_t*)buffer;

    for (size_t i = 0; i < num_blocks; i++)
    {
        regs->blk_size_cnt = (1 << 16) | 512;
        int res = sd_send_cmd(CMD24, (uint32_t)(start_block + i));
        if (res != PERS_SUCCESS)
            return res;

        res = sd_wait_status(STATUS_WRITE_READY, STATUS_WRITE_READY, 500);
        if (res != PERS_SUCCESS)
            return res;

        for (int j = 0; j < 128; j++)
        {
            regs->data = buf[i * 128 + j];
        }

        res = sd_wait_interrupt(INT_DATA_DONE);
        if (res != PERS_SUCCESS)
            return res;
    }

    return PERS_SUCCESS;
}

/* --- Initialization Logic --- */

static int sd_set_clock(uint32_t clock)
{
    unsigned int __attribute__((aligned(16))) mbox[10];
    mbox[0] = 10 * 4;
    mbox[1] = 0;
    mbox[2] = 0x00038002;  // Set clock rate
    mbox[3] = 12;
    mbox[4] = 8;
    mbox[5] = 1;      // EMMC clock ID
    mbox[6] = clock;  // Rate
    mbox[7] = 0;
    mbox[8] = 0;  // End tag
    mbox[9] = 0;

    mbox_call(mbox);
    return mbox[1] == 0x80000000 ? PERS_SUCCESS : -PERS_ERR_IO_ERROR;
}

static int sd_init_host(void)
{
    regs->clk_control |= (7 << 24);
    sleep_ms(20);
    while (regs->clk_control & (7 << 24))
        ;

    regs->int_en = 0xFFFFFFFF;
    regs->int_mask = 0xFFFFFFFF;

    regs->host_control = (regs->host_control & ~0xF00) | 0xE00;  // 3.3V
    sleep_ms(100);
    regs->host_control |= 0x100;

    regs->clk_control = (regs->clk_control & ~0xFFFF) | (0xFA << 8) | 0x01;
    while (!(regs->clk_control & 0x02))
        ;
    regs->clk_control |= 0x04;
    sleep_ms(20);

    return PERS_SUCCESS;
}

static int sd_init_card(void)
{
    if (sd_send_cmd(CMD0, 0) != PERS_SUCCESS)
        return -PERS_ERR_IO_ERROR;
    if (sd_send_cmd(CMD8, 0x1AA) != PERS_SUCCESS)
        return -PERS_ERR_IO_ERROR;

    int timeout = 1000;
    while (timeout--)
    {
        sd_send_cmd(CMD55, 0);
        sd_send_cmd(ACMD41, 0x40FF8000);
        if (regs->resp[0] & 0x80000000)
            break;
        sleep_ms(1);
    }
    if (timeout <= 0)
        return -PERS_ERR_TIMED_OUT;

    if (sd_send_cmd(CMD2, 0) != PERS_SUCCESS)
        return -PERS_ERR_IO_ERROR;
    if (sd_send_cmd(CMD3, 0) != PERS_SUCCESS)
        return -PERS_ERR_IO_ERROR;
    sd_rca = regs->resp[0] & 0xFFFF0000;

    if (sd_send_cmd(CMD9, sd_rca) == PERS_SUCCESS)
    {
        uint32_t c_size = ((regs->resp[2] & 0x3F) << 16) | (regs->resp[1] >> 16);
        sd_block_dev.block_count = (c_size + 1) * 1024;
    }

    if (sd_send_cmd(CMD7, sd_rca) != PERS_SUCCESS)
        return -PERS_ERR_IO_ERROR;
    if (sd_send_cmd(CMD16, 512) != PERS_SUCCESS)
        return -PERS_ERR_IO_ERROR;

    return PERS_SUCCESS;
}

void sd_init(void)
{
    const char* probe_targets[] = {"emmc2", "emmc2_qemu", "emmc", NULL};
    struct pht_node* node = NULL;

    for (int i = 0; probe_targets[i]; i++)
    {
        struct pht_node* n = pht_find_device(probe_targets[i]);
        if (!n)
            continue;

        sdhci_regs_t* r = (sdhci_regs_t*)P2V(n->address[0]);
        if (r->status & STATUS_CARD_INSERT)
        {
            node = n;
            regs = r;
            break;
        }
    }

    if (!node)
    {
        for (int i = 0; probe_targets[i]; i++)
        {
            node = pht_find_device(probe_targets[i]);
            if (node)
            {
                regs = (sdhci_regs_t*)P2V(node->address[0]);
                break;
            }
        }
    }

    if (!node)
        return;

    printf("[ SD ] Initializing controller at %s... ", node->name);

    if (sd_set_clock(100000000) < 0)
    {
        printf("Clock Failed\n");
        return;
    }

    if (sd_init_host() == PERS_SUCCESS && sd_init_card() == PERS_SUCCESS)
    {
        sd_block_dev.block_size = 512;
        sd_block_dev.read_blocks = sd_read_blocks;
        sd_block_dev.write_blocks = sd_write_blocks;
        sd_block_dev.present = 1;
        strncpy(sd_block_dev.name, "sd0", sizeof(sd_block_dev.name));

        block_device_register(&sd_block_dev);

        size_t mb = (sd_block_dev.block_count * 512) / (1024 * 1024);
        printf("OK (%lu MB)\n", mb);
    }
    else
    {
        printf("Failed\n");
    }
}
