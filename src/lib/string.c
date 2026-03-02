#include "string.h"

unsigned int strlen(const char* str)
{
    unsigned int size = 0;
    for (; *str != '\0'; str++, size++)
        ;
    return size;
}

char* strcpy(char* dest, const char* src)
{
    for (; (*dest = *src) != '\0'; dest++, src++)
        ;
    return dest;
}

char* strncpy(char* dest, const char* src, unsigned int count)
{
    unsigned int len = 0;
    for (; len < count && (*dest = *src) != '\0'; dest++, src++, len++)
        ;
    return dest;
}

char* strcat(char* dest, const char* src)
{
    for (; *dest != '\0'; dest++)
        ;
    for (; (*dest = *src) != '\0'; dest++, src++)
        ;
    *dest = '\0';

    return dest;
}

char* strncat(char* dest, const char* src, unsigned int count)
{
    unsigned int len = 0;
    for (; *dest != '\0'; dest++)
        ;
    for (; len < count && (*dest = *src) != '\0'; dest++, src++, len++)
        ;
    *dest = '\0';

    return dest;
}

int strcmp(const char* lhs, const char* rhs)
{
    for (; *lhs == *rhs; lhs++, rhs++)
        if (*lhs == '\0')
            return 0;
    return *lhs - *rhs;
}

int strncmp(const char* lhs, const char* rhs, unsigned int count)
{
    unsigned int len = 0;
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
    return '\0';
}

char* strrchr(const char* str, int c)
{
    char* last = '\0';
    for (; *str != '\0'; str++)
        if (*str == (char)c)
            last = (char*)str;
    return last;
}

char* strstr(const char* haystack, const char* needle)
{
    unsigned int needle_len = strlen(needle);
    if (needle_len == 0)
        return (char*)haystack;
    for (; *haystack != '\0'; haystack++)
    {
        if (*haystack == *needle)
        {
            unsigned int i;
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

int memcmp(const void* ptr1, const void* ptr2, unsigned int num)
{
    const unsigned char* p1 = (const unsigned char*)ptr1;
    const unsigned char* p2 = (const unsigned char*)ptr2;
    for (unsigned int i = 0; i < num; i++, p1++, p2++)
        if (*p1 != *p2)
            return *p1 - *p2;
    return 0;
}

void* memset(void* dest, int val, unsigned int num)
{
    char* src = (char*)dest;
    while (num--)
    {
        *src++ = (char)val;
    }
    return dest;
}

void* memcpy(void* dest, const void* src, unsigned int count)
{
    char* destination = (char*)dest;
    while (count--)
    {
        *destination++ = *(char*)src++;
    }
    return dest;
}

void* memmove(void* dest, const void* src, unsigned int count)
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
