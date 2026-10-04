/*
 * time.h - Clocks and high-resolution sleeping.
 */

#ifndef PERSPICUA_LIBC_TIME_H
#define PERSPICUA_LIBC_TIME_H

#include "uapi/time.h"

int clock_gettime(clockid_t clk_id, struct timespec *tp);
int nanosleep(const struct timespec *req, struct timespec *rem);

#endif // PERSPICUA_LIBC_TIME_H
