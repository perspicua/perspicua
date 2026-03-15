/*
 * stdio.h - Standard Input/Output definitions.
 *
 * This file provides the interface for formatted output operations,
 * primarily used for kernel debugging and basic user application output.
 */

#ifndef PERSPICUA_LIBC_STDIO_H
#define PERSPICUA_LIBC_STDIO_H

#include "types.h"

/*
 * printf - Performs formatted output to the system console.
 * Supported format specifiers include %d, %u, %x, %ld, %lu, %lx, %p, %s, and %c.
 */
void printf(const char* fmt, ...);

#endif /* PERSPICUA_LIBC_STDIO_H */
