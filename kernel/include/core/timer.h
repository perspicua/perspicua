/*
 * timer.h - Public API for system time and interrupt control.
 *
 * This header defines the interface for uptime retrieval, delays, and
 * low-level CPU interrupt management.
 */

#ifndef PERSPICUA_CORE_TIMER_H
#define PERSPICUA_CORE_TIMER_H

/* --- Function Prototypes --- */

/*
 * get_system_time - Returns the uptime in milliseconds.
 */
unsigned long get_system_time(void);

/*
 * sleep_ms - Performs a busy-wait delay for the specified duration.
 */
void sleep_ms(unsigned long ms);

/*
 * timer_interrupt_init - Configures the periodic generic timer tick.
 */
void timer_interrupt_init(void);

/*
 * timer_interrupt_reset - Reloads the timer for the next tick.
 */
void timer_interrupt_reset(void);

/*
 * enable_interrupts - Unmasks IRQs on the local CPU core.
 */
void enable_interrupts(void);

/*
 * disable_interrupts - Masks IRQs on the local CPU core.
 */
void disable_interrupts(void);

/*
 * irq_save - Disables interrupts and returns the previous state mask.
 */
unsigned long irq_save(void);

/*
 * irq_restore - Restores the interrupt state from a saved mask.
 */
void irq_restore(unsigned long flags);

#endif /* PERSPICUA_CORE_TIMER_H */
