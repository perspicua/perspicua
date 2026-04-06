/*
 * stdlib.h - Standard library functions for memory management and utility.
 */

#ifndef PERSPICUA_LIBC_STDLIB_H
#define PERSPICUA_LIBC_STDLIB_H

#include "types.h"

/* Memory management */
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

/* Process control */
__attribute__((noreturn)) void exit(int status);

/* Environment variables */
extern char **environ;
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *string);

/* String to number conversion */
int atoi(const char *nptr);
long atol(const char *nptr);

#endif /* PERSPICUA_LIBC_STDLIB_H */
