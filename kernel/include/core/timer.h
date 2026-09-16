/*
 * timer.h - Public API for system time.
 */

#ifndef PERSPICUA_CORE_TIMER_H
#define PERSPICUA_CORE_TIMER_H

#include "uapi/types.h"

unsigned long timer_get_system_time(void);

/*
 * Seconds since the Unix epoch, from the build date plus uptime -- there is no
 * RTC to ask. Good enough to stamp a file with; not a source of real time.
 */
time_t timer_get_wall_time(void);

// Seconds since the Unix epoch for a local date and time, proleptic Gregorian.
time_t timer_civil_to_epoch(int year, int month, int day, int hour, int min, int sec);

void timer_sleep_ms(unsigned long ms);

void timer_interrupt_init(void);

void timer_interrupt_reset(void);

#endif // PERSPICUA_CORE_TIMER_H
