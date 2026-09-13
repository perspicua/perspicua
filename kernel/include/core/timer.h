/*
 * timer.h - Public API for system time.
 */

#ifndef PERSPICUA_CORE_TIMER_H
#define PERSPICUA_CORE_TIMER_H

unsigned long timer_get_system_time(void);

void timer_sleep_ms(unsigned long ms);

void timer_interrupt_init(void);

void timer_interrupt_reset(void);

#endif // PERSPICUA_CORE_TIMER_H
