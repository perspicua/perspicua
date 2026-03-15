/*
 * timer.h - Public API for system time and interrupt control.
 *
 * This file defines the interface for millisecond-precision timing,
 * system uptime retrieval, and low-level CPU interrupt management.
 */

#ifndef PERSPICUA_KERNEL_TIMER_H
#define PERSPICUA_KERNEL_TIMER_H

/*
 * sleep_ms - Blocks the current CPU core for the specified duration in
 * milliseconds using a busy-wait loop.
 */
void sleep_ms(unsigned long ms);

/*
 * get_system_time - Returns the number of milliseconds elapsed since
 * the system was booted.
 */
unsigned long get_system_time(void);

/*
 * timer_interrupt_init - Configures the ARM generic physical timer
 * to trigger periodic interrupts on the current CPU core.
 */
void timer_interrupt_init(void);

/*
 * timer_interrupt_reset - Resets the timer's compare value to trigger
 * the next periodic interrupt.
 */
void timer_interrupt_reset(void);

/*
 * enable_interrupts - Unmasks IRQ interrupts on the local CPU core.
 */
void enable_interrupts(void);

/*
 * disable_interrupts - Masks IRQ interrupts on the local CPU core.
 */
void disable_interrupts(void);

/*
 * irq_save - Disables interrupts on the local core and returns the
 * previous interrupt state mask.
 */
unsigned long irq_save(void);

/*
 * irq_restore - Restores the local CPU core's interrupt state to the
 * provided mask value.
 */
void irq_restore(unsigned long flags);

#endif /* PERSPICUA_KERNEL_TIMER_H */
