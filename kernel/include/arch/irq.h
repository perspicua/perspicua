/*
 * irq.h - Interrupt masking and the IRQ handler table.
 */

#ifndef PERSPICUA_ARCH_IRQ_H
#define PERSPICUA_ARCH_IRQ_H

#include <stddef.h>
#include <stdint.h>

#include "arch/cpu.h"

// Handler-table size: the BCM2711's highest line in use, the SD controller's, is 158.
#define IRQ_MAX 256

// A handler never calls sched_schedule(): RESCHED asks the dispatcher to, after the GIC's EOI.
typedef enum {
    IRQ_HANDLED = 0,
    IRQ_HANDLED_RESCHED,
} irq_result_t;

typedef irq_result_t (*irq_handler_t)(void *ctx);

// count is per core, so the hottest path in the kernel needs no atomic.
struct irq_desc {
    irq_handler_t handler;
    void *ctx;
    const char *name;
    uint64_t count[CPU_MAX_CORES];
};

void enable_interrupts(void);

void disable_interrupts(void);

unsigned long irq_save(void);

void irq_restore(unsigned long flags);

// True if DAIF's I bit is clear.
static inline int irqs_enabled(void)
{
    unsigned long daif;
    asm volatile("mrs %0, daif" : "=r"(daif));
    return (daif & (1 << 7)) == 0;
}

// ctx is passed back to the handler, name shows in /proc/interrupts; -errno if taken or invalid.
int request_irq(unsigned int irq, irq_handler_t handler, void *ctx, const char *name);

// The caller masks the line at the GIC first, so nothing fires against a cleared entry.
int free_irq(unsigned int irq);

// An unclaimed line still reports IRQ_HANDLED, so the GIC closes it rather than leave it pending.
irq_result_t irq_dispatch(unsigned int irq);

// NULL if irq is out of range.
const struct irq_desc *irq_get_desc(unsigned int irq);

// Non-zero while this core is in an IRQ handler.
int irq_in_handler(void);

#endif // PERSPICUA_ARCH_IRQ_H
