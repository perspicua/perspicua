/*
 * stdlib.c - Allocator-independent stdlib helpers.
 */

#include "stdlib.h"

#include <limits.h>

#include "ctype.h"

#ifndef __KERNEL__
    #include "errno.h"
#endif

static int digit_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10;
    }
    return 99;
}

/*
 * Parses [space][+|-][0x]digits in base (0 picks 8, 10 or 16 from the prefix)
 * as an unsigned magnitude, saturating at limit. *end, if given, is set past
 * the last digit used, or to s when there was none.
 */
static unsigned long parse_magnitude(const char *s, char **end, int base, unsigned long limit,
                                     int *negative, int *overflow)
{
    const char *p = s;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    *negative = *p == '-';
    if (*p == '-' || *p == '+') {
        p++;
    }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')
        && digit_value(p[2]) < 16) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }

    unsigned long value = 0;
    const char *first = p;
    *overflow = 0;
    if (base >= 2 && base <= 36) {
        for (int d; (d = digit_value(*p)) < base; p++) {
            if (value > (limit - (unsigned long)d) / (unsigned long)base) {
                *overflow = 1;
                value = limit;
            } else if (!*overflow) {
                value = value * (unsigned long)base + (unsigned long)d;
            }
        }
    }
    if (end) {
        *end = (char *)(p == first ? s : p);
    }
    return value;
}

long strtol(const char *nptr, char **endptr, int base)
{
    int negative, overflow;
    // A negative number may reach one past LONG_MAX: LONG_MIN's magnitude.
    unsigned long limit = (unsigned long)LONG_MAX + 1;
    unsigned long mag = parse_magnitude(nptr, endptr, base, limit, &negative, &overflow);
    if (!negative && mag > (unsigned long)LONG_MAX) {
        mag = (unsigned long)LONG_MAX;
        overflow = 1;
    }
    if (overflow) {
#ifndef __KERNEL__
        errno = ERANGE;
#endif
        return negative ? LONG_MIN : LONG_MAX;
    }
    return negative ? (long)(0 - mag) : (long)mag;
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
    int negative, overflow;
    unsigned long mag = parse_magnitude(nptr, endptr, base, ULONG_MAX, &negative, &overflow);
    if (overflow) {
#ifndef __KERNEL__
        errno = ERANGE;
#endif
        return ULONG_MAX;
    }
    // As C has it: a minus sign negates the result in unsigned arithmetic.
    return negative ? 0 - mag : mag;
}

int atoi(const char *nptr)
{
    return (int)strtol(nptr, NULL, 10);
}

long atol(const char *nptr)
{
    return strtol(nptr, NULL, 10);
}

int abs(int j)
{
    return j < 0 ? -j : j;
}

long labs(long j)
{
    return j < 0 ? -j : j;
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *))
{
    const char *lo = base;
    while (nmemb > 0) {
        const char *mid = lo + (nmemb / 2) * size;
        int c = compar(key, mid);
        if (c == 0) {
            return (void *)mid;
        }
        if (c > 0) {
            lo = mid + size;
            nmemb -= nmemb / 2 + 1;
        } else {
            nmemb /= 2;
        }
    }
    return NULL;
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
