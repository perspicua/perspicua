/*
 * mailbox.c - Implementation of the VideoCore Mailbox driver.
 *
 * This module handles register-based communication with the GPU's mailbox
 * interface, including cache maintenance to ensure data visibility.
 */

#include "driver/mailbox.h"

#include "stdio.h"
#include "panic.h"

#include "mm/addr.h"
#include "devicetree/fdt.h"

/* --- Private Macros --- */

#define MBOX_STATUS_FULL  0x80000000
#define MBOX_STATUS_EMPTY 0x40000000

/* --- Private Variables --- */

static volatile unsigned int *mbox_read = NULL;
static volatile unsigned int *mbox_status = NULL;
static volatile unsigned int *mbox_write = NULL;

/* --- Public API Implementations --- */

/*
 * mbox_init - Locates and maps the BCM2835 mailbox registers.
 */
void mbox_init(void)
{
    const uint32_t *mbox_node = fdt_find_node_by_compatible("brcm,bcm2835-mbox");
    if (!mbox_node) {
        PANIC("MBOX: device node not found");
    }

    struct fdt_property reg_prop;
    if (fdt_get_property(mbox_node, "reg", &reg_prop) != 0) {
        PANIC("MBOX: missing 'reg' property");
    }

    const uint32_t *reg_data = (const uint32_t *)reg_prop.value;
    uint32_t phys_base = fdt32_to_cpu(reg_data[0]);

    if (phys_base < 0xFC000000) {
        phys_base = (phys_base & 0x01FFFFFF) | 0xFE000000;
    }

    uintptr_t vbase = P2V(phys_base);

    mbox_read = (unsigned int *)(vbase + 0x00);
    mbox_status = (unsigned int *)(vbase + 0x18);
    mbox_write = (unsigned int *)(vbase + 0x20);

    pr_info("mbox: VideoCore mailbox initialized\n");
}

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
            /* Invalidate buffer in cache to read the GPU's response from RAM */
            for (unsigned long i = 0; i < size; i += 64) {
                asm volatile("dc ivac, %0" : : "r"(addr + i));
            }
            asm volatile("dsb sy");
            asm volatile("isb");
            return;
        }
    }
}
