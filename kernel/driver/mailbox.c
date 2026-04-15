/*
 * mailbox.c - Implementation of the VideoCore Mailbox driver.
 *
 * This module handles register-based communication with the GPU's mailbox
 * interface, including cache maintenance to ensure data visibility.
 */

#include "driver/mailbox.h"
#include "driver/device.h"

#include "stdio.h"
#include "panic.h"

#include "mm/addr.h"
#include "devicetree/fdt.h"

#define MBOX_STATUS_FULL  0x80000000
#define MBOX_STATUS_EMPTY 0x40000000

static volatile unsigned int *mbox_read = NULL;
static volatile unsigned int *mbox_status = NULL;
static volatile unsigned int *mbox_write = NULL;

/*
 * bcm2835_mbox_probe - Locates and maps the BCM2835 mailbox registers.
 */
static int bcm2835_mbox_probe(struct device *dev)
{
    uintptr_t vbase = devm_get_io_base(dev, 0);
    if (!vbase) {
        PANIC("MBOX: missing or invalid 'reg' property");
    }

    mbox_read = (unsigned int *)(vbase + 0x00);
    mbox_status = (unsigned int *)(vbase + 0x18);
    mbox_write = (unsigned int *)(vbase + 0x20);

    return 0;
}

CORE_DRIVER(bcm2835_mbox) = {
    .name = "bcm2835-mbox",
    .compatible = "brcm,bcm2835-mbox",
    .probe = bcm2835_mbox_probe,
};

/*
 * mbox_call - Submits a property buffer to the GPU and waits for a response.
 */
void mbox_call(unsigned int *buffer)
{
    unsigned long size = (unsigned long)buffer[0];
    unsigned long addr = (unsigned long)buffer;

    /* Flush request data to RAM so the GPU sees the current buffer contents */
    for (unsigned long i = 0; i < size; i += 64) {
        asm volatile("dc cvac, %0" : : "r"(addr + i));
    }
    asm volatile("dsb sy");

    /* Channel 8 is the standard property channel */
    unsigned int request = (unsigned int)((V2P(buffer) | 0xC0000000) & ~0xF) | 8;

    while (*mbox_status & MBOX_STATUS_FULL) {
        asm volatile("nop");
    }

    *mbox_write = request;

    while (1) {
        while (*mbox_status & MBOX_STATUS_EMPTY) {
            asm volatile("nop");
        }

        unsigned int response = *mbox_read;

        if ((response & 0xF) == 8) {
            /* Clean+invalidate to read the GPU's response from RAM without
             * discarding dirty data in cache lines shared with adjacent
             * stack variables. */
            for (unsigned long i = 0; i < size; i += 64) {
                asm volatile("dc civac, %0" : : "r"(addr + i));
            }
            asm volatile("dsb sy");
            asm volatile("isb");
            return;
        }
    }
}
