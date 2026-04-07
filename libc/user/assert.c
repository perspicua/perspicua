/*
 * assert.c - User-space assertion failure handler.
 */

#include "assert.h"
#include "stdio.h"
#include "stdlib.h"

__attribute__((noreturn)) void __assert_fail(const char *expr, const char *file, int line,
                                             const char *func)
{
    fprintf(stderr, "%s:%d: %s: Assertion `%s' failed.\n", file, line, func, expr);
    exit(1);
}
