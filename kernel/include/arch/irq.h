/*
 * irq.h - Interrupt masking on the calling core.
 */

#ifndef PERSPICUA_ARCH_IRQ_H
#define PERSPICUA_ARCH_IRQ_H

void enable_interrupts(void);

void disable_interrupts(void);

unsigned long irq_save(void);

void irq_restore(unsigned long flags);

#endif
