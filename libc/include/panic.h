/*
 * panic.h - Error handling and assertion primitives.
 *
 * This file defines the kernel panic and assertion macros used to halt
 * execution in the event of unrecoverable system errors.
 */

#ifndef PERSPICUA_LIBC_PANIC_H
#define PERSPICUA_LIBC_PANIC_H

/* Global flag set when the kernel enters a panic state */
extern volatile int kernel_panicked;

/*
 * panic - Primary error handler that halts all CPU cores.
 * @msg: Error message to display.
 * @file: Source file where the panic occurred.
 * @line: Line number where the panic occurred.
 */
void panic(const char* msg, const char* file, int line) __attribute__((noreturn));

/* Standard panic macro providing source location context */
#define PANIC(msg) panic((msg), __FILE__, __LINE__)

/* Standard assertion macro that triggers a panic if condition is false */
#define ASSERT(cond)                                            \
    do                                                          \
    {                                                           \
        if (!(cond))                                            \
        {                                                       \
            panic("ASSERT failed: " #cond, __FILE__, __LINE__); \
        }                                                       \
    } while (0)

#endif /* PERSPICUA_LIBC_PANIC_H */
