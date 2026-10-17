/*
 * exit.c - Process exit and the handlers registered to run at it.
 */

#include "signal.h"
#include "stdio.h"
#include "stdlib.h"

#include "unistd.h"

#define ATEXIT_MAX 16

static void (*atexit_handlers[ATEXIT_MAX])(void);
static int atexit_count;

int atexit(void (*fn)(void))
{
    if (!fn || atexit_count == ATEXIT_MAX) {
        return -1;
    }
    atexit_handlers[atexit_count++] = fn;
    return 0;
}

void exit(int status)
{
    // Popped before the call, so a handler that calls exit() resumes with the rest.
    while (atexit_count > 0) {
        void (*fn)(void) = atexit_handlers[--atexit_count];
        fn();
    }
    fflush(stdout);
    _exit(status);
}

void abort(void)
{
    // A handler may run and return; the program ends all the same, as if SIGABRT killed it.
    sigset_t abrt = 1u << (SIGABRT - 1);
    sigprocmask(SIG_UNBLOCK, &abrt, NULL);
    kill(getpid(), SIGABRT);
    signal(SIGABRT, SIG_DFL);
    kill(getpid(), SIGABRT);
    _exit(128 + SIGABRT);
}
