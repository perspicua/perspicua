/*
 * mailbox.c - Implementation of the VideoCore Mailbox driver.
 *
 * This file handles the low-level register-based communication with the
 * GPU's mailbox interface, including necessary cache maintenance operations.
 */

#include "driver/mailbox.h"

#include "panic.h"
#include "addr.h"

#include "devicetree/pht.h"

/* Mailbox Register Pointers (Static) */
static volatile unsigned int* mbox_read = (void*)0;
static volatile unsigned int* mbox_status = (void*)0;
static volatile unsigned int* mbox_write = (void*)0;

/* Mailbox Status Flags */
#define MBOX_STATUS_FULL 0x80000000
#define MBOX_STATUS_EMPTY 0x40000000

/*
 * mbox_init - Discovers and initializes the mailbox base address.
 */
void mbox_init(void)
{
    struct pht_node* mbox_node = pht_find_device("mailbox");
    if (mbox_node == (void*)0)
    {
        PANIC("[ MBOX ] Device node not found in hardware tree!\n");
    }

    uintptr_t vbase = P2V(mbox_node->address[0]);

    // BCM2711 Mailbox register offsets
    mbox_read = (unsigned int*)(vbase + 0x00);
    mbox_status = (unsigned int*)(vbase + 0x18);
    mbox_write = (unsigned int*)(vbase + 0x20);
}

/*
 * mbox_call - Executes a mailbox transaction with the GPU.
 */
void mbox_call(unsigned int* buffer)
{
    // Determine buffer dimensions and ensure GPU sees our request data
    unsigned long size = (unsigned long)buffer[0];
    unsigned long addr = (unsigned long)buffer;

    for (unsigned long i = 0; i < size; i += 64)
    {
        asm volatile("dc cvac, %0" : : "r"(addr + i));
    }
    asm volatile("dsb sy");

    // Prepare request: convert buffer address to physical and mask channel bits
    unsigned int request = (unsigned int)((V2P(buffer) | 0xC0000000) & ~0xF) | 8;

    // Spin until the mailbox is ready to accept a new request
    while (*mbox_status & MBOX_STATUS_FULL)
    {
        asm volatile("nop");
    }

    *mbox_write = request;

    // Poll for the corresponding response on the same channel
    while (1)
    {
        while (*mbox_status & MBOX_STATUS_EMPTY)
        {
            asm volatile("nop");
        }

        unsigned int response = *mbox_read;

        if ((response & 0xF) == 8)
        {
            // Invalidate the cache to see the GPU's modifications to the buffer
            for (unsigned long i = 0; i < size; i += 64)
            {
                asm volatile("dc ivac, %0" : : "r"(addr + i));
            }
            asm volatile("dsb sy");
            asm volatile("isb");
            return;
        }
    }
}
