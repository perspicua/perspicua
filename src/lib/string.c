#include "string.h"
#include "types.h"

size_t strlen(const char* str)
{
    size_t size = 0;
    for (; *str != '\0'; str++, size++)
        ;
    return size;
}

char* strcpy(char* dest, const char* src)
{
    char* start = dest;
    for (; (*dest = *src) != '\0'; dest++, src++)
        ;
    return start;
}

char* strncpy(char* dest, const char* src, size_t count)
{
    char* start = dest;
    size_t len = 0;
    for (; len < count && (*dest = *src) != '\0'; dest++, src++, len++)
        ;
    return start;
}

char* strcat(char* dest, const char* src)
{
    char* start = dest;
    for (; *dest != '\0'; dest++)
        ;
    for (; (*dest = *src) != '\0'; dest++, src++)
        ;
    *dest = '\0';

    return start;
}

char* strncat(char* dest, const char* src, size_t count)
{
    char* start = dest;
    size_t len = 0;
    for (; *dest != '\0'; dest++)
        ;
    for (; len < count && (*dest = *src) != '\0'; dest++, src++, len++)
        ;
    *dest = '\0';

    return start;
}

int strcmp(const char* lhs, const char* rhs)
{
    for (; *lhs == *rhs; lhs++, rhs++)
        if (*lhs == '\0')
            return 0;
    return *lhs - *rhs;
}

int strncmp(const char* lhs, const char* rhs, size_t count)
{
    if (count == 0)
        return 0;

    size_t len = 0;
    for (; len < count - 1 && *lhs == *rhs; lhs++, rhs++, len++)
        if (*lhs == '\0')
            return 0;
    return *lhs - *rhs;
}

char* strchr(const char* str, int c)
{
    for (; *str != '\0'; str++)
        if (*str == (char)c)
            return (char*)str;
    return NULL;
}

char* strrchr(const char* str, int c)
{
    char* last = NULL;
    for (; *str != '\0'; str++)
        if (*str == (char)c)
            last = (char*)str;
    return last;
}

char* strstr(const char* haystack, const char* needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len == 0)
        return (char*)haystack;
    for (; *haystack != '\0'; haystack++)
    {
        if (*haystack == *needle)
        {
            size_t i;
            for (i = 1; i < needle_len; i++)
            {
                if (haystack[i] != needle[i])
                    break;
            }
            if (i == needle_len)
                return (char*)haystack;
        }
    }
    return 0;
}

int memcmp(const void* ptr1, const void* ptr2, size_t num)
{
    const unsigned char* p1 = (const unsigned char*)ptr1;
    const unsigned char* p2 = (const unsigned char*)ptr2;
    for (size_t i = 0; i < num; i++, p1++, p2++)
        if (*p1 != *p2)
            return *p1 - *p2;
    return 0;
}

void* memset(void* dest, int val, size_t num)
{
    uint64_t* d64 = (uint64_t*)dest;

    uint64_t v8 = (unsigned char)val;
    uint64_t v64 = v8 | (v8 << 8) | (v8 << 16) | (v8 << 24) | (v8 << 32) | (v8 << 40) | (v8 << 48) | (v8 << 56);

    while (num >= 8)
    {
        *d64++ = v64;
        num -= 8;
    }

    char* d8 = (char*)d64;
    while (num--)
        *d8++ = (char)val;

    return dest;
}

void* memcpy(void* dest, const void* src, size_t count)
{
    uint64_t* d64 = (uint64_t*)dest;
    const uint64_t* s64 = (const uint64_t*)src;

    while (count >= 8)
    {
        *d64++ = *s64++;
        count -= 8;
    }

    char* d8 = (char*)d64;
    const char* s8 = (const char*)s64;
    while (count--)
        *d8++ = *s8++;

    return dest;
}

void* memmove(void* dest, const void* src, size_t count)
{
    char* destination = (char*)dest;
    char* source = (char*)src;
    if (destination < source)
    {
        while (count--)
        {
            *destination++ = *source++;
        }
    }
    else if (destination > source)
    {
        destination += count;
        source += count;
        while (count--)
        {
            *--destination = *--source;
        }
    }
    return dest;
}
