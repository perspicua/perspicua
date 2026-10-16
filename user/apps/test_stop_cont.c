/*
 * test_stop_cont.c - Job-control signals: SIGSTOP, SIGCONT, SIGKILL, sessions and SIGTTIN.
 */

#include <signal.h>
#include <syscall.h>
#include <wait.h>

#include "utest.h"

#define STATUS_KILLED (128 + SIGKILL)

// Polls for up to a second; returns the signal that stopped pid, or 0.
static int wait_stopped(int pid)
{
    for (int i = 0; i < 100; i++) {
        int status = 0;
        int r = waitpid(pid, &status, WNOHANG | WUNTRACED);
        if (r == pid && WIFSTOPPED(status)) {
            return WSTOPSIG(status);
        }
        if (r != 0) {
            return 0;
        }
        usleep(10 * 1000);
    }
    return 0;
}

static int kill_and_reap(int pid)
{
    int status = -1;
    kill(pid, SIGKILL);
    if (waitpid(pid, &status, 0) != pid) {
        return -1;
    }
    return status;
}

// Writes a byte down fd every 20 ms until killed.
static void ticker(int fd)
{
    while (1) {
        write(fd, "x", 1);
        usleep(20 * 1000);
    }
}

static int drain(int fd)
{
    char buf[64];
    int total = 0, n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        total += n;
    }
    return total;
}

static void test_observable_stop_cont(void)
{
    int fds[2];
    CHECK(pipe(fds) == 0);

    int pid = fork();
    if (pid == 0) {
        close(fds[0]);
        ticker(fds[1]);
    }
    close(fds[1]);
    CHECK(pid > 0);

    char c;
    CHECK(read(fds[0], &c, 1) == 1);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);

    CHECK(kill(pid, SIGSTOP) == 0);
    CHECK(wait_stopped(pid) == SIGSTOP);
    drain(fds[0]);
    usleep(200 * 1000);
    CHECK(drain(fds[0]) == 0);

    CHECK(kill(pid, SIGCONT) == 0);
    usleep(200 * 1000);
    CHECK(drain(fds[0]) > 0);

    CHECK(kill_and_reap(pid) == STATUS_KILLED);
    close(fds[0]);
}

static void test_kill_stopped_task(void)
{
    int pid = fork();
    if (pid == 0) {
        while (1) {
            usleep(50 * 1000);
        }
    }
    CHECK(pid > 0);

    CHECK(kill(pid, SIGSTOP) == 0);
    CHECK(wait_stopped(pid) == SIGSTOP);
    CHECK(kill_and_reap(pid) == STATUS_KILLED);
}

static void test_cont_racing_stop(void)
{
    int fds[2];
    CHECK(pipe(fds) == 0);

    int pid = fork();
    if (pid == 0) {
        close(fds[0]);
        ticker(fds[1]);
    }
    close(fds[1]);
    CHECK(pid > 0);

    usleep(100 * 1000);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    drain(fds[0]);

    CHECK(kill(pid, SIGSTOP) == 0);
    CHECK(kill(pid, SIGCONT) == 0);
    usleep(300 * 1000);
    CHECK(drain(fds[0]) > 0);

    CHECK(kill_and_reap(pid) == STATUS_KILLED);
    close(fds[0]);
}

// A signal to the caller's group reaches its member but not a child that left for its own session.
static void test_session_daemon(void)
{
    int ready[2];
    CHECK(pipe(ready) == 0);
    signal(SIGINT, SIG_IGN);

    int daemon_pid = fork();
    if (daemon_pid == 0) {
        signal(SIGINT, SIG_DFL);
        int sid = setsid();
        if (sid != getpid() || setsid() >= 0) {
            _exit(3);
        }
        write(ready[1], "d", 1);
        while (1) {
            usleep(50 * 1000);
        }
    }

    int member_pid = fork();
    if (member_pid == 0) {
        signal(SIGINT, SIG_DFL);
        write(ready[1], "m", 1);
        while (1) {
            usleep(50 * 1000);
        }
    }
    CHECK(daemon_pid > 0 && member_pid > 0);

    char c;
    CHECK(read(ready[0], &c, 1) == 1 && read(ready[0], &c, 1) == 1);
    CHECK(setpgid(daemon_pid, getpid()) < 0);

    CHECK(kill(0, SIGINT) == 0);
    int status = -1;
    CHECK(waitpid(member_pid, &status, 0) == member_pid);
    CHECK(status == 128 + SIGINT);
    CHECK(waitpid(daemon_pid, &status, WNOHANG) == 0);

    CHECK(kill_and_reap(daemon_pid) == STATUS_KILLED);
    signal(SIGINT, SIG_DFL);
    close(ready[0]);
    close(ready[1]);
}

/*
 * A background group must not steal terminal input: a reader that can be stopped is,
 * and one that ignores SIGTTIN gets an error instead of looping on a read nothing wakes.
 */
static void test_background_read(void)
{
    int ign_pid = fork();
    if (ign_pid == 0) {
        setpgid(0, 0);
        signal(SIGTTIN, SIG_IGN);
        char c;
        int r = read(0, &c, 1);
        _exit(r < 0 ? 42 : 43);
    }
    CHECK(ign_pid > 0);

    int status = -1;
    CHECK(waitpid(ign_pid, &status, 0) == ign_pid);
    CHECK(status == 42);

    int stop_pid = fork();
    if (stop_pid == 0) {
        setpgid(0, 0);
        char c;
        read(0, &c, 1);
        _exit(44);
    }
    CHECK(stop_pid > 0);

    CHECK(wait_stopped(stop_pid) == SIGTTIN);
    CHECK(kill_and_reap(stop_pid) == STATUS_KILLED);
}

int main(void)
{
    test_observable_stop_cont();
    test_kill_stopped_task();
    test_cont_racing_stop();
    test_session_daemon();
    test_background_read();

    return utest_summary("test_stop_cont");
}
