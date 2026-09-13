/*
 * irq.c - Interrupt masking and the IRQ handler table.
 */

#include "arch/irq.h"

#include "uapi/errors.h"

static struct irq_desc irq_table[IRQ_MAX];

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

int request_irq(unsigned int irq, irq_handler_t handler, void *ctx, const char *name)
{
    if (irq >= IRQ_MAX || handler == NULL) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }
    if (irq_table[irq].handler != NULL) {
        return -PERS_ERR_ALREADY_EXISTS;
    }

    irq_table[irq].handler = handler;
    irq_table[irq].ctx = ctx;
    irq_table[irq].name = name;
    return 0;
}

irq_result_t irq_dispatch(unsigned int irq)
{
    if (irq >= IRQ_MAX || irq_table[irq].handler == NULL) {
        return IRQ_HANDLED;
    }

    irq_table[irq].count[cpu_id()]++;
    return irq_table[irq].handler(irq_table[irq].ctx);
}

const struct irq_desc *irq_get_desc(unsigned int irq)
{
    if (irq >= IRQ_MAX) {
        return NULL;
    }
    return &irq_table[irq];
}
