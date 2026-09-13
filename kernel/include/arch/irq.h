/*
 * irq.h - Interrupt masking and the IRQ handler table.
 */

#ifndef PERSPICUA_ARCH_IRQ_H
#define PERSPICUA_ARCH_IRQ_H

#include "types.h"

#include "arch/cpu.h"

/*
 * Entries in the handler table. The GIC addresses up to 1020 interrupt IDs,
 * but the BCM2711 uses far fewer - the SD controller, the highest line in use,
 * sits at 158.
 */
#define IRQ_MAX 256

/*
 * What a handler reports back to the dispatcher.
 *
 * A handler must never call schedule() itself: the dispatcher still owes the
 * GIC its end-of-interrupt write, and schedule() does not return. Returning
 * IRQ_HANDLED_RESCHED asks for that reschedule once the line is closed.
 */
typedef enum {
    IRQ_HANDLED = 0,
    IRQ_HANDLED_RESCHED,
} irq_result_t;

typedef irq_result_t (*irq_handler_t)(void *ctx);

/*
 * struct irq_desc - One interrupt line: who owns it, and how often it fired.
 *
 * count is kept per core so the dispatcher needs no atomic on the hottest path
 * in the kernel; a core only ever writes its own slot.
 */
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

/*
 * request_irq - Claims an interrupt line for a driver.
 *
 * ctx is handed back to the handler untouched, and name is what
 * /proc/interrupts shows. Returns 0, or a negative error if the number is out
 * of range or the line already has an owner.
 */
int request_irq(unsigned int irq, irq_handler_t handler, void *ctx, const char *name);

/*
 * irq_dispatch - Runs the handler that claimed irq, counting the hit.
 *
 * Reports IRQ_HANDLED for a line nobody claimed, so an unexpected interrupt is
 * still closed at the GIC rather than left pending.
 */
irq_result_t irq_dispatch(unsigned int irq);

/*
 * irq_get_desc - Returns NULL if irq is out of range.
 */
const struct irq_desc *irq_get_desc(unsigned int irq);

#endif // PERSPICUA_ARCH_IRQ_H
