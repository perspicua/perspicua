/*
 * utest.h - CHECK counts and carries on; utest_summary prints the line run_tests.sh gates on.
 */

#ifndef PERSPICUA_UTEST_H
#define PERSPICUA_UTEST_H

#include <stddef.h>

#include "stdio.h"
#include "string.h"
#include "sys/wait.h"
#include "unistd.h"

static int utest_passed, utest_failed;

#define CHECK(expr) utest_check((expr) != 0, #expr, __LINE__)

static inline void utest_check(int ok, const char *what, int line)
{
    if (ok) {
        utest_passed++;
    } else {
        utest_failed++;
        printf("  [FAILED] line %d: %s\n", line, what);
    }
}

// A check named for what it shows rather than by its expression.
static inline void utest_named(const char *name, int ok)
{
    if (ok) {
        utest_passed++;
    } else {
        utest_failed++;
        printf("  [FAILED] %s\n", name);
    }
}

// Where utest_run feeds its child's stdin from input, and where stderr joins stdout.
#define UTEST_STDIN     1
#define UTEST_MERGE_ERR 2

// Runs child(), or execvp(argv) when child is NULL, in a fork with its stdout read into out and
// *len set to the bytes kept. Output past size is drained and dropped. Returns the exit status.
static inline int utest_run(const char *input, char *out, int size, int *len, void (*child)(void),
                            char *const argv[], int flags)
{
    int in[2] = {-1, -1}, outp[2];
    if (pipe(outp) != 0 || ((flags & UTEST_STDIN) && pipe(in) != 0)) {
        return -1;
    }
    int pid = fork();
    if (pid == 0) {
        if (flags & UTEST_STDIN) {
            dup2(in[0], 0);
            close(in[0]);
            close(in[1]);
        }
        dup2(outp[1], 1);
        if (flags & UTEST_MERGE_ERR) {
            dup2(outp[1], 2);
        }
        close(outp[0]);
        close(outp[1]);
        if (child) {
            child();
        } else {
            execvp(argv[0], argv);
        }
        _exit(127);
    }
    close(outp[1]);
    if (flags & UTEST_STDIN) {
        close(in[0]);
        if (input) {
            write(in[1], input, strlen(input));
        }
        close(in[1]);
    }

    int n = 0;
    char spill[256];
    for (;;) {
        int room = size - 1 - n;
        int r =
            room > 0 ? read(outp[0], out + n, (size_t)room) : read(outp[0], spill, sizeof(spill));
        if (r <= 0) {
            break;
        }
        if (room > 0) {
            n += r;
        }
    }
    out[n] = '\0';
    close(outp[0]);
    if (len) {
        *len = n;
    }

    int status = -1;
    waitpid(pid, &status, 0);
    return status;
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
