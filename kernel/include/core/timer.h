/*
 * timer.h - Public API for system time and interrupt control.
 */

#ifndef PERSPICUA_CORE_TIMER_H
#define PERSPICUA_CORE_TIMER_H

unsigned long get_system_time(void);

void sleep_ms(unsigned long ms);

void timer_interrupt_init(void);

void timer_interrupt_reset(void);

void enable_interrupts(void);

void disable_interrupts(void);

unsigned long irq_save(void);

void irq_restore(unsigned long flags);

#endif // PERSPICUA_CORE_TIMER_H
