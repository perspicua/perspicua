#ifndef _STRING_H_
#define _STRING_H_

#include "lib/types.h"

size_t strlen(const char* str);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t count);

char* strcat(char* dest, const char* src);
char* strncat(char* dest, const char* src, size_t count);

int strcmp(const char* lhs, const char* rhs);
int strncmp(const char* lhs, const char* rhs, size_t count);

char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* haystack, const char* needle);

size_t strspn(const char* s, const char* accept);
size_t strcspn(const char* s, const char* reject);
char* strtok(char* str, const char* delim);
char* strtok_r(char* str, const char* delim, char** saveptr);

int memcmp(const void* ptr1, const void* ptr2, size_t num);
void* memset(void* dest, int val, size_t num);
void* memcpy(void* dest, const void* src, size_t count);
void* memmove(void* dest, const void* src, size_t count);

#endif // _STRING_H_
