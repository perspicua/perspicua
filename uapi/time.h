/*
 * time.h
 *
 * This header defines time related structures
 */

#ifndef PERSPICUA_UAPI_TIME_H
#define PERSPICUA_UAPI_TIME_H

#include "types.h"

struct timeval {
    time_t tv_sec;
    long tv_usec;
}; /* micro seconds */
struct timespec {
    time_t tv_sec;
    long tv_nsec;
}; /* nano seconds  */

typedef int clockid_t;

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

#endif // PERSPICUA_UAPI_TIME_H
