/*
 * string.c - Implementation of string and memory manipulation utilities.
 *
 * This file provides standard C string functions and optimized memory
 * operations (memset, memcpy, memmove) for the Perspicua OS.
 */

#include "string.h"

#include "types.h"
#include "stdlib.h"

#ifdef __KERNEL__
    #include "core/lock.h"
/* Global synchronization for thread-unsafe string tokenization. */
static spinlock_t strtok_lock = SPINLOCK_INIT;
#endif

/* --- Public API Implementations --- */

size_t strlen(const char *str)
{
    size_t size = 0;
    while (str && *str != '\0') {
        str++;
        size++;
    }
    return size;
}

size_t strnlen(const char *str, size_t size)
{
    if (!str) {
        return 0;
    }

    size_t sz = 0;
    while (sz < size && str[sz] != '\0') {
        sz++;
    }
    return sz;
}

char *strcpy(char *dest, const char *src)
{
    char *start = dest;
    while ((*dest++ = *src++) != '\0') {
        asm volatile("nop");
    }
    return start;
}

char *strncpy(char *dest, const char *src, size_t count)
{
    char *start = dest;
    size_t len = 0;
    while (len < count && (*dest = *src) != '\0') {
        dest++;
        src++;
        len++;
    }
    while (len < count) {
        *dest++ = '\0';
        len++;
    }
    return start;
}

char *strcat(char *dest, const char *src)
{
    char *start = dest;
    while (*dest != '\0') {
        dest++;
    }
    while ((*dest++ = *src++) != '\0') {
        asm volatile("nop");
    }
    return start;
}

char *strncat(char *dest, const char *src, size_t count)
{
    char *start = dest;
    size_t len = 0;
    while (*dest != '\0') {
        dest++;
    }
    while (len < count && (*dest = *src) != '\0') {
        dest++;
        src++;
        len++;
    }
    *dest = '\0';
    return start;
}

int strcmp(const char *lhs, const char *rhs)
{
    while (*lhs && (*lhs == *rhs)) {
        lhs++;
        rhs++;
    }
    return *(unsigned char *)lhs - *(unsigned char *)rhs;
}

int strncmp(const char *lhs, const char *rhs, size_t count)
{
    if (count == 0) {
        return 0;
    }
    size_t len = 0;
    while (len < count - 1 && *lhs && (*lhs == *rhs)) {
        lhs++;
        rhs++;
        len++;
    }
    return *(unsigned char *)lhs - *(unsigned char *)rhs;
}

char *strchr(const char *str, int c)
{
    while (str) {
        if (*str == (char)c) {
            return (char *)str;
        }
        if (*str == '\0') {
            break;
        }
        str++;
    }
    return NULL;
}

char *strrchr(const char *str, int c)
{
    char *last = NULL;
    while (str) {
        if (*str == (char)c) {
            last = (char *)str;
        }
        if (*str == '\0') {
            break;
        }
        str++;
    }
    return last;
}

char *strstr(const char *haystack, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len == 0) {
        return (char *)haystack;
    }
    while (*haystack != '\0') {
        if (*haystack == *needle) {
            size_t i;
            for (i = 1; i < needle_len; i++) {
                if (haystack[i] != needle[i]) {
                    break;
                }
            }
            if (i == needle_len) {
                return (char *)haystack;
            }
        }
        haystack++;
    }
    return NULL;
}

size_t strspn(const char *s, const char *accept)
{
    size_t count = 0;
    while (s && *s && strchr(accept, *s++)) {
        count++;
    }
    return count;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t count = 0;
    while (s && *s && !strchr(reject, *s++)) {
        count++;
    }
    return count;
}

char *strdup(const char *str)
{
    if (str == NULL) {
        return NULL;
    }

    size_t len = strlen(str) + 1;
    char *dup = malloc(len);

    if (dup == NULL) {
        return NULL;
    }

    return memcpy(dup, str, len);
}

char *strndup(const char *str, size_t size)
{
    if (str == NULL) {
        return NULL;
    }

    size_t len = strnlen(str, size);

    char *dup = malloc(len + 1);
    if (dup == NULL) {
        return NULL;
    }

    dup[len] = '\0';
    return memcpy(dup, str, len);
}

char *strtok_r(char *str, const char *delim, char **saveptr)
{
    if (!str) {
        str = *saveptr;
    }

    if (!str || *str == '\0') {
        *saveptr = NULL;
        return NULL;
    }

    str += strspn(str, delim);
    if (*str == '\0') {
        *saveptr = NULL;
        return NULL;
    }

    char *end = str + strcspn(str, delim);
    if (*end == '\0') {
        *saveptr = NULL;
    } else {
        *end = '\0';
        *saveptr = end + 1;
    }

    return str;
}

char *strtok(char *str, const char *delim)
{
    static char *last_token;
#ifdef __KERNEL__
    unsigned long flags = spin_lock_irqsave(&strtok_lock);
    char *result = strtok_r(str, delim, &last_token);
    spin_unlock_irqrestore(&strtok_lock, flags);
    return result;
#else
    return strtok_r(str, delim, &last_token);
#endif
}

int memcmp(const void *ptr1, const void *ptr2, size_t num)
{
    const unsigned char *p1 = (const unsigned char *)ptr1;
    const unsigned char *p2 = (const unsigned char *)ptr2;
    for (size_t i = 0; i < num; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }
    return 0;
}

void *memset(void *dest, int val, size_t num)
{
    unsigned char *d8 = (unsigned char *)dest;
    unsigned char v8 = (unsigned char)val;

    while (num && ((uintptr_t)d8 & 7)) {
        *d8++ = v8;
        num--;
    }

    uint64_t v64 = (uint64_t)v8 | ((uint64_t)v8 << 8) | ((uint64_t)v8 << 16) | ((uint64_t)v8 << 24)
                   | ((uint64_t)v8 << 32) | ((uint64_t)v8 << 40) | ((uint64_t)v8 << 48)
                   | ((uint64_t)v8 << 56);
    uint64_t *d64 = (uint64_t *)d8;
    while (num >= 8) {
        *d64++ = v64;
        num -= 8;
    }

    d8 = (unsigned char *)d64;
    while (num--) {
        *d8++ = v8;
    }

    return dest;
}

void *memcpy(void *dest, const void *src, size_t count)
{
    unsigned char *d8 = (unsigned char *)dest;
    const unsigned char *s8 = (const unsigned char *)src;

    uint64_t *d64 = (uint64_t *)d8;
    const uint64_t *s64 = (const uint64_t *)s8;

    while (count >= 8) {
        *d64++ = *s64++;
        count -= 8;
    }

    d8 = (unsigned char *)d64;
    s8 = (const unsigned char *)s64;
    while (count--) {
        *d8++ = *s8++;
    }

    return dest;
}

void *memmove(void *dest, const void *src, size_t count)
{
    unsigned char *d8 = (unsigned char *)dest;
    const unsigned char *s8 = (const unsigned char *)src;

    if (d8 < s8) {
        uint64_t *d64 = (uint64_t *)d8;
        const uint64_t *s64 = (const uint64_t *)s8;
        while (count >= 8) {
            *d64++ = *s64++;
            count -= 8;
        }
        d8 = (unsigned char *)d64;
        s8 = (const unsigned char *)s64;
        while (count--) {
            *d8++ = *s8++;
        }
    } else if (d8 > s8) {
        d8 += count;
        s8 += count;
        uint64_t *d64 = (uint64_t *)d8;
        const uint64_t *s64 = (const uint64_t *)s8;
        while (count >= 8) {
            *--d64 = *--s64;
            count -= 8;
        }
        d8 = (unsigned char *)d64;
        s8 = (const unsigned char *)s64;
        while (count--) {
            *--d8 = *--s8;
        }
    }
    return dest;
}
