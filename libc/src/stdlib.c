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

static void swap_bytes(char *a, char *b, size_t size)
{
    while (size--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void sift_down(char *base, size_t root, size_t end, size_t size,
                      int (*compar)(const void *, const void *))
{
    for (;;) {
        size_t child = 2 * root + 1;
        if (child >= end) {
            return;
        }
        if (child + 1 < end && compar(base + child * size, base + (child + 1) * size) < 0) {
            child++;
        }
        if (compar(base + root * size, base + child * size) >= 0) {
            return;
        }
        swap_bytes(base + root * size, base + child * size, size);
        root = child;
    }
}

// Heapsort: O(n log n) in the worst case, and no recursion to overflow a small stack.
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *))
{
    char *b = base;
    if (nmemb < 2) {
        return;
    }
    for (size_t i = nmemb / 2; i-- > 0;) {
        sift_down(b, i, nmemb, size, compar);
    }
    for (size_t end = nmemb - 1; end > 0; end--) {
        swap_bytes(b, b + end * size, size);
        sift_down(b, 0, end, size, compar);
    }
}
