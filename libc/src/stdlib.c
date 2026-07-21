/*
 * stdlib.c - Allocator-independent stdlib helpers.
 *
 * These live apart from malloc.c so that linking them does not drag in an
 * allocator: malloc.c is userspace-only, and the kernel gets its own glue.
 */

#include "stdlib.h"

int atoi(const char *nptr)
{
    int res = 0, sign = 1;
    while (*nptr == ' ')
        nptr++;
    if (*nptr == '-') {
        sign = -1;
        nptr++;
    }
    while (*nptr >= '0' && *nptr <= '9')
        res = res * 10 + (*nptr++ - '0');
    return res * sign;
}

long atol(const char *nptr)
{
    long res = 0, sign = 1;
    while (*nptr == ' ')
        nptr++;
    if (*nptr == '-') {
        sign = -1;
        nptr++;
    }
    while (*nptr >= '0' && *nptr <= '9')
        res = res * 10 + (*nptr++ - '0');
    return res * sign;
}
