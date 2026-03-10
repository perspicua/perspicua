#include "string.h"

size_t strlen(const char* str)
{
    size_t len = 0;
    while (*str++)
        len++;
    return len;
}

char* strcpy(char* dest, const char* src)
{
    char* d = dest;
    while ((*d++ = *src++))
        ;
    return dest;
}

char* strcat(char* dest, const char* src)
{
    char* d = dest;
    while (*d)
        d++;
    while ((*d++ = *src++))
        ;
    return dest;
}

int strcmp(const char* lhs, const char* rhs)
{
    while (*lhs && (*lhs == *rhs))
    {
        lhs++;
        rhs++;
    }
    return *(unsigned char*)lhs - *(unsigned char*)rhs;
}

void* memset(void* dest, int val, size_t num)
{
    unsigned char* d = (unsigned char*)dest;
    while (num--)
        *d++ = (unsigned char)val;
    return dest;
}

int memcmp(const void* ptr1, const void* ptr2, size_t num)
{
    const unsigned char* p1 = (const unsigned char*)ptr1;
    const unsigned char* p2 = (const unsigned char*)ptr2;
    while (num--)
    {
        if (*p1 != *p2)
            return *p1 - *p2;
        p1++;
        p2++;
    }
    return 0;
}
