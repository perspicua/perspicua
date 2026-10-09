/*
 * stdlib.h - Standard library functions for memory management and utility.
 */

#ifndef PERSPICUA_LIBC_STDLIB_H
#define PERSPICUA_LIBC_STDLIB_H

#include <stddef.h>

// Memory management
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

// Process control
__attribute__((noreturn)) void exit(int status);

// Runs fn at exit() or a return from main, last registered first; returns -1 when full.
int atexit(void (*fn)(void));

// Sorts nmemb elements of size bytes; not stable, so ties need a deciding compare.
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));

// Pseudo-random numbers
#define RAND_MAX 0x7fffffff
int rand(void);
void srand(unsigned int seed);

// Environment variables
extern char **environ;
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *string);

// String to number conversion
int atoi(const char *nptr);
long atol(const char *nptr);

#endif // PERSPICUA_LIBC_STDLIB_H
