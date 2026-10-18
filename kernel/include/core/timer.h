/*
 * timer.h - Public API for system time.
 */

#ifndef PERSPICUA_CORE_TIMER_H
#define PERSPICUA_CORE_TIMER_H

#include <stdint.h>

#include "uapi/types.h"

unsigned long timer_get_system_time(void);

// The generic timer's free-running count, and a span of it in milliseconds.
uint64_t timer_ticks(void);
uint64_t timer_ticks_to_ms(uint64_t ticks);

// No RTC: midnight UTC of the build day plus uptime. Good for file stamps, not real time.
time_t timer_get_wall_time(void);

// Seconds since the Unix epoch for a local date and time, proleptic Gregorian.
time_t timer_civil_to_epoch(int year, int month, int day, int hour, int min, int sec);

void timer_sleep_ms(unsigned long ms);

void timer_interrupt_init(void);

#endif // PERSPICUA_CORE_TIMER_H
