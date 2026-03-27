/*
 * stdio.h - Kernel/user formatted output API.
 *
 * This header defines the public interface for formatted I/O operations
 * used across both kernel and user-mode.
 */

#ifndef PERSPICUA_LIBC_STDIO_H
#define PERSPICUA_LIBC_STDIO_H

#include "types.h"

#include <stdarg.h>

/* Formatted output to the console (UART). */
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __KERNEL__
/* Kernel-specific logging that prepends a system timestamp. */
int printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

    #define pr_info(fmt, ...)  printk(fmt, ##__VA_ARGS__)
    #define pr_err(fmt, ...)   printk("ERROR: " fmt, ##__VA_ARGS__)
    #define pr_warn(fmt, ...)  printk("WARNING: " fmt, ##__VA_ARGS__)
    #define pr_debug(fmt, ...) printk("DEBUG: " fmt, ##__VA_ARGS__)
#endif

/* va_list variant of printf. */
int vprintf(const char *fmt, va_list args);

/* Formatted output into a size-bounded buffer. Always NUL-terminates. */
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* va_list variant of snprintf. */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);

#endif /* PERSPICUA_LIBC_STDIO_H */
