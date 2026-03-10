#include "driver/mailbox.h"

#include "devicetree/pht.h"
#include "lib/panic.h"
#include "kernel/addr.h"

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
            return;
    }
}
