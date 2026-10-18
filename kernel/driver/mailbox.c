/*
 * mailbox.c - Driver for the VideoCore mailbox interface.
 */

#include "driver/mailbox.h"

#include <stddef.h>
#include <stdint.h>

#include "stdio.h"

#include "core/lock.h"
#include "devicetree/fdt.h"
#include "driver/device.h"
#include "mm/addr.h"
#include "panic.h"

#define MBOX_STATUS_FULL  0x80000000
#define MBOX_STATUS_EMPTY 0x40000000
#define MBOX_CACHE_LINE   64

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

// Every line the buffer touches, the partial ones at either end included.
static void mbox_dcache(unsigned long addr, unsigned long size, int invalidate)
{
    for (unsigned long line = addr & ~(MBOX_CACHE_LINE - 1UL); line < addr + size;
         line += MBOX_CACHE_LINE) {
        if (invalidate) {
            asm volatile("dc civac, %0" : : "r"(line));
        } else {
            asm volatile("dc cvac, %0" : : "r"(line));
        }
    }
    asm volatile("dsb sy");
}

static void mbox_exchange(unsigned int *buffer)
{
    unsigned long size = (unsigned long)buffer[0];
    unsigned long addr = (unsigned long)buffer;

    // The GPU sees only the first GB, through its uncached 0xC0000000 alias.
    if (V2P(buffer) + size > 0x40000000UL) {
        PANIC("mailbox: buffer outside the GPU's first GB");
    }

    mbox_dcache(addr, size, 0);

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
            // Clean as well, as a stack buffer's lines may also hold its caller's dirty locals.
            mbox_dcache(addr, size, 1);
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
    // A whole cache line, so no neighbour shares it; the message uses the first half.
    static unsigned int __attribute__((aligned(MBOX_CACHE_LINE))) msg[MBOX_CACHE_LINE / 4];

    unsigned long flags = spin_lock_irqsave(&mbox_lock);
    msg[0] = 8 * 4;
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
