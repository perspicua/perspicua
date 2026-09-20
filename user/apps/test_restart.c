/*
 * test_restart.c - What a signal does to a blocking call.
 *
 * Lives in userspace because a handler only runs on the way back to EL0,
 * which a boot-phase kernel test task never reaches.
 */

#include <stdint.h>

#include "errno.h"
#include "signal.h"
#include "stdio.h"
#include "string.h"
#include "sys/wait.h"
#include "time.h"
#include "unistd.h"

// What the child observed, reported through its exit status.
#define CHILD_RESTARTED  0 // read() resumed and delivered the byte
#define CHILD_EINTR      1 // read() failed with EINTR
#define CHILD_NO_HANDLER 2
#define CHILD_OTHER      3

static volatile int handler_runs;

// The handler runs on the way out of this kill(), before it returns.
static void raise_self(int sig)
{
    kill(getpid(), sig);
}

static void on_usr1(int sig)
{
    (void)sig;
    handler_runs++;
}

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

// The sleeps are what guarantee the read is already blocked when the signal
// lands, and still blocked when the byte arrives.
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

static int run_sleep_case(void)
{
    int pid = fork();
    if (pid < 0) {
        printf("test_restart: fork failed\n");
        return -1;
    }

    if (pid == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr1;
        sa.sa_flags = SA_RESTART;
        sigaction(SIGUSR1, &sa, NULL);

        struct timespec req = {.tv_sec = 10, .tv_nsec = 0};
        struct timespec rem = {0, 0};
        errno = 0;
        int n = nanosleep(&req, &rem);

        if (n == 0) {
            _exit(CHILD_OTHER); // slept the whole ten seconds: never interrupted
        }
        if (errno != EINTR || !handler_runs) {
            _exit(CHILD_OTHER);
        }
        _exit(rem.tv_sec >= 8 ? CHILD_EINTR : CHILD_OTHER);
    }

    usleep(200000);
    kill(pid, SIGUSR1);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        printf("test_restart: waitpid failed\n");
        return -1;
    }
    return status & 0xFF;
}

static char alt_area[SIGSTKSZ];
static volatile int handler_sp_on_alt;

static void on_usr2(int sig)
{
    (void)sig;
    char probe;
    uintptr_t sp = (uintptr_t)&probe;
    uintptr_t base = (uintptr_t)alt_area;
    handler_sp_on_alt = (sp >= base && sp < base + sizeof(alt_area));
}

// 0 = ran on the alt stack, 1 = ran on the normal stack, 2 = setup failed.
static int run_altstack_case(void)
{
    int pid = fork();
    if (pid < 0) {
        printf("test_restart: fork failed\n");
        return -1;
    }

    if (pid == 0) {
        stack_t ss = {.ss_sp = alt_area, .ss_flags = 0, .ss_size = sizeof(alt_area)};
        if (sigaltstack(&ss, NULL) < 0) {
            _exit(2);
        }

        stack_t got = {0};
        if (sigaltstack(NULL, &got) < 0 || got.ss_sp != alt_area || got.ss_size != sizeof(alt_area)
            || got.ss_flags != 0) {
            _exit(2);
        }

        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr2;
        sa.sa_flags = SA_ONSTACK;
        sigaction(SIGUSR2, &sa, NULL);

        raise_self(SIGUSR2);
        _exit(handler_sp_on_alt ? 0 : 1);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        printf("test_restart: waitpid failed\n");
        return -1;
    }
    return status & 0xFF;
}

// Without SA_ONSTACK an installed alt stack must be left alone.
static int run_no_onstack_case(void)
{
    int pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        stack_t ss = {.ss_sp = alt_area, .ss_flags = 0, .ss_size = sizeof(alt_area)};
        sigaltstack(&ss, NULL);

        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr2;
        sa.sa_flags = 0;
        sigaction(SIGUSR2, &sa, NULL);

        raise_self(SIGUSR2);
        _exit(handler_sp_on_alt ? 0 : 1);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    return status & 0xFF;
}

// Returns the count of rejections that did not happen.
static int run_altstack_reject_case(void)
{
    int bad = 0;

    stack_t small = {.ss_sp = alt_area, .ss_flags = 0, .ss_size = MINSIGSTKSZ - 1};
    if (sigaltstack(&small, NULL) == 0 || errno != ENOMEM) {
        bad++;
    }

    stack_t flags = {.ss_sp = alt_area, .ss_flags = 0x40, .ss_size = sizeof(alt_area)};
    if (sigaltstack(&flags, NULL) == 0 || errno != EINVAL) {
        bad++;
    }

    stack_t kernel_side = {.ss_sp = (void *)-4096L, .ss_flags = 0, .ss_size = sizeof(alt_area)};
    if (sigaltstack(&kernel_side, NULL) == 0) {
        bad++;
    }

    stack_t off = {.ss_sp = NULL, .ss_flags = SS_DISABLE, .ss_size = 0};
    stack_t got = {0};
    if (sigaltstack(&off, NULL) < 0 || sigaltstack(NULL, &got) < 0 || got.ss_flags != SS_DISABLE) {
        bad++;
    }

    return bad;
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
    failures += check("nanosleep returns EINTR with time owed", run_sleep_case(), CHILD_EINTR);
    failures += check("SA_ONSTACK runs the handler on the alt stack", run_altstack_case(), 0);
    failures += check("without SA_ONSTACK it uses the normal stack", run_no_onstack_case(), 1);
    failures += check("sigaltstack rejects bad stacks", run_altstack_reject_case(), 0);

    if (failures == 0) {
        printf("test_restart: all 6 tests passed\n");
    } else {
        printf("test_restart: %d of 6 tests failed\n", failures);
    }
    return failures != 0;
}
