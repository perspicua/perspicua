#include "driver/mailbox.h"

#include "devicetree/pht.h"
#include "panic.h"
#include "addr.h"

volatile unsigned int* MBOX_READ;
volatile unsigned int* MBOX_STATUS;
volatile unsigned int* MBOX_WRITE;

volatile unsigned int const MBOX_FULL = (unsigned int)0x80000000;
volatile unsigned int const MBOX_EMPTY = (unsigned int)0x40000000;

void mbox_init(void)
{
    struct pht_node* mbox_node = pht_find_device("mailbox");
    if (mbox_node == NULL)
    {
        PANIC("[ MBOX ] Device node not found in hardware tree!\n");
    }

    uintptr_t vbase = P2V(mbox_node->address[0]);
    MBOX_READ = (unsigned int*)(vbase + 0x00);
    MBOX_STATUS = (unsigned int*)(vbase + 0x18);
    MBOX_WRITE = (unsigned int*)(vbase + 0x20);
}

void mbox_call(unsigned int* buffer)
{
    // Clean D-cache for the buffer so the VideoCore sees our request
    unsigned long size = buffer[0];
    unsigned long addr = (unsigned long)buffer;
    for (unsigned long i = 0; i < size; i += 64)
    {
        asm volatile("dc cvac, %0" : : "r"(addr + i));
    }
    asm volatile("dsb sy");

    unsigned int request = (unsigned int)((V2P(buffer) | 0xC0000000) & ~0xF) | 8;
    while (*MBOX_STATUS & MBOX_FULL)
        ;

    *MBOX_WRITE = request;

    while (1)
    {
        while (*MBOX_STATUS & MBOX_EMPTY)
            ;
        unsigned int response = *MBOX_READ;

        if ((response & 0xF) == 8)
        {
            // Invalidate D-cache for the buffer so we see the VideoCore's response
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
