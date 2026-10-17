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
// Ends the program by SIGABRT, whatever a handler for it does.
__attribute__((noreturn)) void abort(void);

// Runs fn at exit() or a return from main, last registered first; returns -1 when full.
int atexit(void (*fn)(void));

// Sorts nmemb elements of size bytes; not stable, so ties need a deciding compare.
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

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

// String to number conversion. strtol and strtoul take base 0 (from the prefix) or 2-36, and
// saturate with ERANGE; atoi and atol are strtol in base 10.
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
int atoi(const char *nptr);
long atol(const char *nptr);

int abs(int j);
long labs(long j);

#endif // PERSPICUA_LIBC_STDLIB_H
