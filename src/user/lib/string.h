#ifndef _LIBUSER_STRING_H_
#define _LIBUSER_STRING_H_

#include "lib/types.h"

size_t strlen(const char* str);
char* strcpy(char* dest, const char* src);
char* strcat(char* dest, const char* src);
int strcmp(const char* lhs, const char* rhs);
void* memset(void* dest, int val, size_t num);
int memcmp(const void* ptr1, const void* ptr2, size_t num);

#endif
