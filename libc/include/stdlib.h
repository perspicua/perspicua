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
void exit(int status);

/* String to number conversion */
int atoi(const char *nptr);
long atol(const char *nptr);

#endif /* PERSPICUA_LIBC_STDLIB_H */
