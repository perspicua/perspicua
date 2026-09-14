#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <syscall.h>
#include <wait.h>

static void test_observable_stop_cont(void)
{
    printf("--- Test: Observable SIGSTOP / SIGCONT ---\n");

    int child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        // Child: infinite loop printing heartbeat
        int i = 0;
        while (1) {
            printf("tick %d\n", i++);
            usleep((100) * 1000);
        }
    } else {
        // Parent
        usleep((250) * 1000);

        printf("[parent sends STOP]\n");
        int res = kill(child_pid, SIGSTOP);
        assert(res == 0);

        usleep((600) * 1000);

        printf("[parent sends CONT]\n");
        res = kill(child_pid, SIGCONT);
        assert(res == 0);

        usleep((250) * 1000);

        printf("[parent sends KILL]\n");
        res = kill(child_pid, SIGKILL);
        assert(res == 0);

        int status = -1;
        res = waitpid(child_pid, &status, 0);
        assert(res == child_pid);
        assert(status == 137);

        printf("Observable SIGSTOP / SIGCONT test passed!\n\n");
    }
}

static void test_kill_stopped_task(void)
{
    printf("--- Test: Kill-a-stopped-task ---\n");

    int child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        while (1) {
            usleep((50) * 1000);
        }
    } else {
        usleep((100) * 1000);

        printf("[parent] sending STOP to child...\n");
        int res = kill(child_pid, SIGSTOP);
        assert(res == 0);

        usleep((200) * 1000);

        printf("[parent] sending KILL directly to stopped child (no CONT)...\n");
        res = kill(child_pid, SIGKILL);
        assert(res == 0);

        int status = -1;
        res = waitpid(child_pid, &status, 0);
        assert(res == child_pid);
        assert(status == 137);

        printf("Kill-a-stopped-task regression test passed! (reaped status 137)\n\n");
    }
}

static void test_cont_racing_stop(void)
{
    printf("--- Test: CONT-racing-STOP ---\n");

    int fds[2];
    int res = pipe(fds);
    assert(res == 0);

    int child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        close(fds[0]);
        while (1) {
            write(fds[1], "x", 1);
            usleep((50) * 1000);
        }
    } else {
        close(fds[1]);
        usleep((100) * 1000);

        // Set non-blocking on pipe read end and drain any initial ticks
        int flags = fcntl(fds[0], F_GETFL, 0);
        fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
        char dummy[64];
        while (read(fds[0], dummy, sizeof(dummy)) > 0) {}

        printf("[parent] sending STOP immediately followed by CONT (tight window)...\n");
        res = kill(child_pid, SIGSTOP);
        assert(res == 0);
        res = kill(child_pid, SIGCONT);
        assert(res == 0);

        // Wait a stretch to verify child continues ticking
        usleep((300) * 1000);

        char buf[64];
        int bytes = read(fds[0], buf, sizeof(buf));
        assert(bytes > 0);
        printf("[parent] verified child is still ticking (%d bytes read from pipe)\n", bytes);

        // Clean up child
        res = kill(child_pid, SIGKILL);
        assert(res == 0);

        int status = -1;
        res = waitpid(child_pid, &status, 0);
        assert(res == child_pid);
        assert(status == 137);

        close(fds[0]);
        printf("CONT-racing-STOP regression test passed!\n\n");
    }
}

static void test_session_daemon(void)
{
    printf("--- Test: Session Daemon & Setsid Immunity ---\n");

    // Child 1: calls setsid to detach into its own session (daemon)
    int daemon_pid = fork();
    assert(daemon_pid >= 0);

    if (daemon_pid == 0) {
        int sid = setsid();
        assert(sid > 0);
        assert(sid == getpid());

        // Verify setsid fails if already a session/group leader
        int err = setsid();
        assert(err < 0);

        while (1) {
            usleep((50) * 1000);
        }
    }

    // Child 2: stays in parent's session/group
    int fg_pid = fork();
    assert(fg_pid >= 0);

    if (fg_pid == 0) {
        while (1) {
            usleep((50) * 1000);
        }
    }

    usleep((100) * 1000);

    // Verify parent cannot setpgid daemon_pid into parent's group across sessions
    int err = setpgid(daemon_pid, getpid());
    assert(err < 0);

    // Send SIGINT to fg_pid
    int res = kill(fg_pid, SIGINT);
    assert(res == 0);

    int status = -1;
    res = waitpid(fg_pid, &status, 0);
    assert(res == fg_pid);
    assert(status == 130);

    // Daemon child must still be alive! Clean it up with KILL
    res = kill(daemon_pid, SIGKILL);
    assert(res == 0);
    status = -1;
    res = waitpid(daemon_pid, &status, 0);
    assert(res == daemon_pid);
    assert(status == 137);

    printf("Session Daemon & Setsid Immunity test passed!\n\n");
}

/*
 * A background group must not steal terminal input. Both outcomes are checked:
 * a reader that can be stopped is, and one that has blocked the signal gets an
 * error rather than looping on a read it can never be woken from.
 */
static void test_background_read(void)
{
    printf("--- Test: Background read raises SIGTTIN ---\n");

    // Child that ignores SIGTTIN must fail the read instead of stopping.
    int ign_pid = fork();
    assert(ign_pid >= 0);

    if (ign_pid == 0) {
        setpgid(0, 0);
        signal(SIGTTIN, SIG_IGN);
        char c;
        int r = read(0, &c, 1);
        _exit(r < 0 ? 42 : 43);
    }

    int status = -1;
    assert(waitpid(ign_pid, &status, 0) == ign_pid);
    assert(status == 42);

    // Child that leaves SIGTTIN at default must stop, not exit.
    int stop_pid = fork();
    assert(stop_pid >= 0);

    if (stop_pid == 0) {
        setpgid(0, 0);
        char c;
        read(0, &c, 1);
        _exit(44);
    }

    usleep((200) * 1000);
    assert(waitpid(stop_pid, &status, WNOHANG) == 0);

    assert(kill(stop_pid, SIGKILL) == 0);
    status = -1;
    assert(waitpid(stop_pid, &status, 0) == stop_pid);
    assert(status == 137);

    printf("Background read SIGTTIN test passed!\n\n");
}

int main(void)
{
    printf("=== Running SIGSTOP / SIGCONT & Signal Regression Tests ===\n\n");

    test_observable_stop_cont();
    test_kill_stopped_task();
    test_cont_racing_stop();
    test_session_daemon();
    test_background_read();

    printf("=== All SIGSTOP / SIGCONT & Signal Regression Tests Passed! ===\n");
    return 0;
}
