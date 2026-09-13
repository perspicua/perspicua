/*
 * stdlib.c - Allocator-independent stdlib helpers.
 */

#include "stdlib.h"

int atoi(const char *nptr)
{
    int res = 0, sign = 1;
    while (*nptr == ' ') {
        nptr++;
    }
    if (*nptr == '-') {
        sign = -1;
        nptr++;
    }
    while (*nptr >= '0' && *nptr <= '9') {
        res = res * 10 + (*nptr++ - '0');
    }
    return res * sign;
}

long atol(const char *nptr)
{
    long res = 0, sign = 1;
    while (*nptr == ' ') {
        nptr++;
    }
    if (*nptr == '-') {
        sign = -1;
        nptr++;
    }
    while (*nptr >= '0' && *nptr <= '9') {
        res = res * 10 + (*nptr++ - '0');
    }
    return res * sign;
}
