#ifndef PERSPICUA_LIBC_STDLIB_H
#define PERSPICUA_LIBC_STDLIB_H

#include "types.h"

void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);

#endif /* PERSPICUA_LIBC_STDLIB_H */
