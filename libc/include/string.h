/*
 * string.h - Standard string and memory manipulation utilities.
 *
 * This file provides the definitions for common operations on C-style
 * strings and raw memory blocks, following standard library conventions.
 */

#ifndef PERSPICUA_LIBC_STRING_H
#define PERSPICUA_LIBC_STRING_H

#include "types.h"

/* String length and copying */
size_t strlen(const char *str);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t count);

/* String concatenation */
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t count);

/* String comparison */
int strcmp(const char *lhs, const char *rhs);
int strncmp(const char *lhs, const char *rhs, size_t count);

/* String searching and tokenization */
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);

size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strtok(char *str, const char *delim);
char *strtok_r(char *str, const char *delim, char **saveptr);

/* Raw memory operations */
int memcmp(const void *ptr1, const void *ptr2, size_t num);
void *memset(void *dest, int val, size_t num);
void *memcpy(void *dest, const void *src, size_t count);
void *memmove(void *dest, const void *src, size_t count);

#endif /* PERSPICUA_LIBC_STRING_H */
