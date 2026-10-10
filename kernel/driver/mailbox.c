/*
 * mailbox.c - Driver for the VideoCore mailbox interface.
 */

#include "driver/mailbox.h"
#include "driver/device.h"

#include <stddef.h>
#include <stdint.h>

#include "stdio.h"
#include "panic.h"

#include "core/lock.h"
#include "mm/addr.h"
#include "devicetree/fdt.h"

#define MBOX_STATUS_FULL  0x80000000
#define MBOX_STATUS_EMPTY 0x40000000

static volatile unsigned int *mbox_read = NULL;
static volatile unsigned int *mbox_status = NULL;
static volatile unsigned int *mbox_write = NULL;

// One request in flight at a time: a reply is matched by its channel alone.
static spinlock_t mbox_lock = SPINLOCK_INIT;

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

static void mbox_exchange(unsigned int *buffer)
{
    unsigned long size = (unsigned long)buffer[0];
    unsigned long addr = (unsigned long)buffer;

    // The GPU sees only the first GB, through its uncached 0xC0000000 alias.
    if (V2P(buffer) + size > 0x40000000UL) {
        PANIC("mailbox: buffer outside the GPU's first GB");
    }

    // Flush request data to RAM so the GPU sees the current buffer contents
    for (unsigned long i = 0; i < size; i += 64) {
        asm volatile("dc cvac, %0" : : "r"(addr + i));
    }
    asm volatile("dsb sy");

    // Channel 8 is the standard property channel
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

void mbox_call(unsigned int *buffer)
{
    unsigned long flags = spin_lock_irqsave(&mbox_lock);
    mbox_exchange(buffer);
    spin_unlock_irqrestore(&mbox_lock, flags);
}

int mbox_query(uint32_t tag, uint32_t value[2])
{
    // Static, so it lives in the kernel image inside the GPU's first GB; a task's stack may not.
    static unsigned int __attribute__((aligned(16))) msg[8];

    unsigned long flags = spin_lock_irqsave(&mbox_lock);
    msg[0] = sizeof(msg);
    msg[1] = 0;
    msg[2] = tag;
    msg[3] = 8;
    msg[4] = 0;
    msg[5] = value[0];
    msg[6] = value[1];
    msg[7] = 0;
    mbox_exchange(msg);
    // The firmware sets bit 31 of a tag's length word once it has answered that tag.
    int answered = msg[1] == 0x80000000 && (msg[4] & 0x80000000);
    value[0] = msg[5];
    value[1] = msg[6];
    spin_unlock_irqrestore(&mbox_lock, flags);
    return answered ? 0 : -1;
}
