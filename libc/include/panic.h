/*
 * panic.h - Error handling and assertion primitives.
 *
 * This file defines the kernel panic and assertion macros used to halt
 * execution in the event of unrecoverable system errors.
 */
#ifndef PERSPICUA_LIBC_PANIC_H
#define PERSPICUA_LIBC_PANIC_H

#include "arch/exception.h" /* struct exception_trap_frame */

/* Global flag set when the kernel enters a panic state */
extern volatile int kernel_panicked;

/*
 * panic_full - Primary error handler; accepts an optional trap frame.
 *
 * @msg:  Human-readable error message.
 * @file: Source file (__FILE__).
 * @line: Line number (__LINE__).
 * @fp:   Frame pointer at the call site for stack unwinding.
 * @tf:   Trap frame from an exception handler, or NULL if not available.
 *        When non-NULL the trap frame register state is printed instead of
 *        the live EL1 snapshot, giving the actual faulting context.
 */
void panic_full(const char *msg, const char *file, int line, unsigned long fp,
                struct exception_trap_frame *tf) __attribute__((noreturn));

/*
 * PANIC(msg) - Trigger a kernel panic with source location and stack trace.
 */
#define PANIC(msg) \
    panic_full((msg), __FILE__, __LINE__, (unsigned long)__builtin_frame_address(0), (void *)0)

/*
 * PANIC_TF(msg, tf) - Panic with a trap frame from an exception handler.
 * Prefer this over PANIC() inside exception handlers so the faulting
 * register state is printed rather than the live EL1 snapshot.
 */
#define PANIC_TF(msg, tf) \
    panic_full((msg), __FILE__, __LINE__, (unsigned long)__builtin_frame_address(0), (tf))

/*
 * ASSERT(cond) - Panic if condition is false, printing the failed expression.
 */
#define ASSERT(cond)                                                          \
    do {                                                                      \
        if (__builtin_expect(!(cond), 0)) {                                   \
            panic_full("ASSERT failed: " #cond, __FILE__, __LINE__,           \
                       (unsigned long)__builtin_frame_address(0), (void *)0); \
        }                                                                     \
    } while (0)

/*
 * ASSERT_MSG(cond, msg) - Panic with a custom message if condition is false.
 */
#define ASSERT_MSG(cond, msg)                                                                \
    do {                                                                                     \
        if (__builtin_expect(!(cond), 0)) {                                                  \
            panic_full((msg), __FILE__, __LINE__, (unsigned long)__builtin_frame_address(0), \
                       (void *)0);                                                           \
        }                                                                                    \
    } while (0)

#endif /* PERSPICUA_LIBC_PANIC_H */
