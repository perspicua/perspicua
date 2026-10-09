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

static unsigned long rand_state;
static int rand_seeded;

void srand(unsigned int seed)
{
    // splitmix64 spreads small seeds across the whole state.
    unsigned long z = (unsigned long)seed + 0x9e3779b97f4a7c15UL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9UL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebUL;
    z ^= z >> 31;
    rand_state = z ? z : 1;
    rand_seeded = 1;
}

int rand(void)
{
    if (!rand_seeded) {
        srand(1);
    }

    // xorshift64*
    rand_state ^= rand_state >> 12;
    rand_state ^= rand_state << 25;
    rand_state ^= rand_state >> 27;
    return (int)((rand_state * 0x2545f4914f6cdd1dUL) >> 33);
}
