/*
 * stdio.h - Kernel/user formatted output API.
 */
#ifndef PERSPICUA_LIBC_STDIO_H
#define PERSPICUA_LIBC_STDIO_H

#include "types.h"
#include <stdarg.h>

/*
 * printf - Formatted output to the console (UART).
 *   \n is translated to \r\n automatically.
 *   Thread-safe under __KERNEL__ via printf_lock.
 */
int printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/*
 * printk - Kernel-specific logging that prepends a system timestamp.
 */
#ifdef __KERNEL__
int printk(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

    #define pr_info(fmt, ...)  printk(fmt, ##__VA_ARGS__)
    #define pr_err(fmt, ...)   printk("ERROR: " fmt, ##__VA_ARGS__)
    #define pr_warn(fmt, ...)  printk("WARNING: " fmt, ##__VA_ARGS__)
    #define pr_debug(fmt, ...) printk("DEBUG: " fmt, ##__VA_ARGS__)
#endif

/*
 * vprintf - va_list variant of printf.
 */
int vprintf(const char* fmt, va_list args);

/*
 * snprintf - Formatted output into a size-bounded buffer.
 *   Always NUL-terminates. Returns the number of bytes that would have been
 *   written if the buffer were unlimited (C99 semantics).
 *   Does NOT translate \n to \r\n — produces clean strings.
 */
int snprintf(char* buf, size_t size, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

/*
 * vsnprintf - va_list variant of snprintf.
 */
int vsnprintf(char* buf, size_t size, const char* fmt, va_list args);

#endif /* PERSPICUA_LIBC_STDIO_H */
