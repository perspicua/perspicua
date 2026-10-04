/*
 * sys/time.h - Wall-clock time.
 */

#ifndef PERSPICUA_LIBC_SYS_TIME_H
#define PERSPICUA_LIBC_SYS_TIME_H

#include "uapi/time.h"

int gettimeofday(struct timeval *tv, void *tz);

#endif // PERSPICUA_LIBC_SYS_TIME_H
