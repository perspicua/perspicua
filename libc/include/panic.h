/*
 * panic.h - Error handling and assertion primitives.
 *
 * This file defines the kernel panic and assertion macros used to halt
 * execution in the event of unrecoverable system errors.
 */

#ifndef PERSPICUA_LIBC_PANIC_H
#define PERSPICUA_LIBC_PANIC_H

#include "arch/exception.h"

/* Global flag set when the kernel enters a panic state. */
extern volatile int kernel_panicked;

const char *panic_resolve_symbol(unsigned long addr, unsigned long *offset);

/* Primary error handler; accepts an optional trap frame. */
void panic_full(const char *msg, const char *file, int line, unsigned long fp,
                struct exception_trap_frame *tf) __attribute__((noreturn));

/* Trigger a kernel panic with source location and stack trace. */
#define PANIC(msg) \
    panic_full((msg), __FILE__, __LINE__, (unsigned long)__builtin_frame_address(0), (void *)0)

/* Panic with a trap frame from an exception handler. */
#define PANIC_TF(msg, tf) \
    panic_full((msg), __FILE__, __LINE__, (unsigned long)__builtin_frame_address(0), (tf))

/* Panic if condition is false, printing the failed expression. */
#define ASSERT(cond)                                                          \
    do {                                                                      \
        if (__builtin_expect(!(cond), 0)) {                                   \
            panic_full("ASSERT failed: " #cond, __FILE__, __LINE__,           \
                       (unsigned long)__builtin_frame_address(0), (void *)0); \
        }                                                                     \
    } while (0)

/* Panic with a custom message if condition is false. */
#define ASSERT_MSG(cond, msg)                                                                \
    do {                                                                                     \
        if (__builtin_expect(!(cond), 0)) {                                                  \
            panic_full((msg), __FILE__, __LINE__, (unsigned long)__builtin_frame_address(0), \
                       (void *)0);                                                           \
        }                                                                                    \
    } while (0)

#endif /* PERSPICUA_LIBC_PANIC_H */
