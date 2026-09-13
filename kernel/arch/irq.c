/*
 * irq.c - Interrupt masking on the calling core.
 */

#include "arch/irq.h"

void enable_interrupts(void)
{
    asm volatile("msr daifclr, #2");
}

void disable_interrupts(void)
{
    asm volatile("msr daifset, #2");
}

unsigned long irq_save(void)
{
    unsigned long flags;
    asm volatile("mrs %0, daif" : "=r"(flags));
    asm volatile("msr daifset, #2");
    return flags;
}

void irq_restore(unsigned long flags)
{
    asm volatile("msr daif, %0" : : "r"(flags));
}
