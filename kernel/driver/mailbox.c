/*
 * mailbox.c - Implementation of the VideoCore Mailbox driver.
 *
 * This file handles the low-level register-based communication with the
 * GPU's mailbox interface, including necessary cache maintenance operations.
 */

#include "driver/mailbox.h"

#include "panic.h"
#include "mm/addr.h"

#include "devicetree/fdt.h"

#include "stdio.h"
/* Mailbox Register Pointers (Static) */
static volatile unsigned int *mbox_read = NULL;
static volatile unsigned int *mbox_status = NULL;
static volatile unsigned int *mbox_write = NULL;

/* Mailbox Status Flags */
#define MBOX_STATUS_FULL  0x80000000
#define MBOX_STATUS_EMPTY 0x40000000

/*
 * mbox_init - Discovers and initializes the mailbox base address.
 */
void mbox_init(void)
{
    const uint32_t *mbox_node = fdt_find_node_by_compatible("brcm,bcm2835-mbox");
    if (!mbox_node) {
        PANIC("[ MBOX ] Device node not found in DTB!\n");
    }

    struct fdt_property reg_prop;
    if (fdt_get_property(mbox_node, "reg", &reg_prop) != 0) {
        PANIC("[ MBOX ] Missing 'reg' property in DTB!\n");
    }

    const uint32_t *reg_data = (const uint32_t *)reg_prop.value;
    uint32_t phys_base = fdt32_to_cpu(reg_data[0]);
    if (phys_base < 0xFC000000) {
        phys_base = (phys_base & 0x01FFFFFF) | 0xFE000000;
    }

    uintptr_t vbase = P2V(phys_base);

    // BCM2711 Mailbox register offsets
    mbox_read = (unsigned int *)(vbase + 0x00);
    mbox_status = (unsigned int *)(vbase + 0x18);
    mbox_write = (unsigned int *)(vbase + 0x20);

    pr_info("mbox: VideoCore mailbox initialized\n");
}

/*
 * mbox_call - Executes a mailbox transaction with the GPU.
 */
void mbox_call(unsigned int *buffer)
{
    // Determine buffer dimensions and ensure GPU sees our request data
    unsigned long size = (unsigned long)buffer[0];
    unsigned long addr = (unsigned long)buffer;

    for (unsigned long i = 0; i < size; i += 64) {
        asm volatile("dc cvac, %0" : : "r"(addr + i));
    }
    asm volatile("dsb sy");

    // Prepare request: convert buffer address to physical and mask channel bits
    unsigned int request = (unsigned int)((V2P(buffer) | 0xC0000000) & ~0xF) | 8;

    // Spin until the mailbox is ready to accept a new request
    while (*mbox_status & MBOX_STATUS_FULL) {
        asm volatile("nop");
    }

    *mbox_write = request;

    // Poll for the corresponding response on the same channel
    while (1) {
        while (*mbox_status & MBOX_STATUS_EMPTY) {
            asm volatile("nop");
        }

        unsigned int response = *mbox_read;

        if ((response & 0xF) == 8) {
            // Invalidate the cache to see the GPU's modifications to the buffer
            for (unsigned long i = 0; i < size; i += 64) {
                asm volatile("dc ivac, %0" : : "r"(addr + i));
            }
            asm volatile("dsb sy");
            asm volatile("isb");
            return;
        }
    }
}
