/*
 * assert.h - Diagnostic assertion support.
 *
 * In kernel mode, assert() delegates to the existing ASSERT() macro from
 * panic.h, which triggers a full kernel panic with register dump.
 *
 * In user mode, a failed assertion prints a diagnostic message to stderr
 * and exits with status 1 via __assert_fail().
 *
 * Define NDEBUG before including this header to compile out all assertions.
 */

#ifndef PERSPICUA_LIBC_ASSERT_H
#define PERSPICUA_LIBC_ASSERT_H

#ifdef __KERNEL__

    #include "panic.h"

    #ifdef NDEBUG
        #define assert(cond) ((void)0)
    #else
        #define assert(cond) ASSERT(cond)
    #endif

#else /* user space */

    #ifdef NDEBUG
        #define assert(cond) ((void)0)
    #else
__attribute__((noreturn)) void __assert_fail(const char *expr, const char *file, int line,
                                             const char *func);

        #define assert(cond)                                            \
            do {                                                        \
                if (__builtin_expect(!(cond), 0))                       \
                    __assert_fail(#cond, __FILE__, __LINE__, __func__); \
            } while (0)
    #endif

#endif /* __KERNEL__ */

#endif /* PERSPICUA_LIBC_ASSERT_H */
