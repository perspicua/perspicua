/*
 * utest.h - CHECK counts and carries on; utest_summary prints the line run_tests.sh gates on.
 */

#ifndef PERSPICUA_UTEST_H
#define PERSPICUA_UTEST_H

#include "stdio.h"

static int utest_passed, utest_failed;

#define CHECK(expr) utest_check((expr) != 0, #expr, __LINE__)

static inline void utest_check(int ok, const char *what, int line)
{
    if (ok) {
        utest_passed++;
    } else {
        utest_failed++;
        printf("  FAIL line %d: %s\n", line, what);
    }
}

// Returns main's exit status: 0 only when every check passed.
static inline int utest_summary(const char *suite)
{
    if (utest_failed) {
        printf("%s: %d of %d tests failed\n", suite, utest_failed, utest_passed + utest_failed);
        return 1;
    }
    printf("%s: all %d tests passed\n", suite, utest_passed);
    return 0;
}

#endif // PERSPICUA_UTEST_H
