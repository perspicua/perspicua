/*
 * test_restart.c - Checks what a signal does to a blocking read.
 *
 * The kernel suites cannot reach this: a handler only runs on the way back to
 * EL0, which a boot-phase test task never does. Each case forks a child that
 * blocks reading a pipe, signals it, and then feeds the pipe; the child
 * reports what its read() returned.
 */

#include <stdint.h>

#include "errno.h"
#include "signal.h"
#include "stdio.h"
#include "string.h"
#include "sys/wait.h"
#include "unistd.h"

// What the child observed, reported through its exit status.
#define CHILD_RESTARTED  0 // read() resumed and delivered the byte
#define CHILD_EINTR      1 // read() failed with EINTR
#define CHILD_NO_HANDLER 2
#define CHILD_OTHER      3

static volatile int handler_runs;

static void on_usr1(int sig)
{
    (void)sig;
    handler_runs++;
}

/*
 * child_body - Blocks on the pipe and classifies how the read ended.
 */
static __attribute__((noreturn)) void child_body(int rfd, int wfd, int flags)
{
    close(wfd);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sa.sa_flags = flags;
    sigaction(SIGUSR1, &sa, NULL);

    char c = 0;
    errno = 0;
    int n = read(rfd, &c, 1);

    if (n == 1 && c == 'x') {
        _exit(handler_runs ? CHILD_RESTARTED : CHILD_NO_HANDLER);
    }
    if (n < 0 && errno == EINTR) {
        _exit(handler_runs ? CHILD_EINTR : CHILD_NO_HANDLER);
    }
    _exit(CHILD_OTHER);
}

/*
 * run_case - Interrupts a blocked read and returns the child's verdict.
 *
 * The sleeps give the child time to reach the read before the signal, and to
 * run its handler before the byte arrives; without that gap the read could
 * succeed without ever being interrupted, which proves nothing.
 */
static int run_case(int flags)
{
    int fds[2];
    if (pipe(fds) < 0) {
        printf("test_restart: pipe failed\n");
        return -1;
    }

    int pid = fork();
    if (pid < 0) {
        printf("test_restart: fork failed\n");
        return -1;
    }
    if (pid == 0) {
        child_body(fds[0], fds[1], flags);
    }

    close(fds[0]);
    usleep(200000);
    kill(pid, SIGUSR1);
    usleep(200000);
    write(fds[1], "x", 1);
    close(fds[1]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        printf("test_restart: waitpid failed\n");
        return -1;
    }
    return status & 0xFF;
}

static int check(const char *name, int got, int want)
{
    if (got == want) {
        printf("  [ok]     %s\n", name);
        return 0;
    }
    printf("  [FAILED] %s: expected %d, got %d\n", name, want, got);
    return 1;
}

int main(void)
{
    printf("test_restart: a signal arriving mid-read\n");

    int failures = 0;
    failures += check("SA_RESTART resumes the read", run_case(SA_RESTART), CHILD_RESTARTED);
    failures += check("without SA_RESTART it fails with EINTR", run_case(0), CHILD_EINTR);

    if (failures == 0) {
        printf("test_restart: all 2 tests passed\n");
    } else {
        printf("test_restart: %d of 2 tests failed\n", failures);
    }
    return failures != 0;
}
