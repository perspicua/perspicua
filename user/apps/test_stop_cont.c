#include <assert.h>
#include <signals.h>
#include <stdio.h>
#include <syscall.h>

static void test_observable_stop_cont(void)
{
    printf("--- Test: Observable SIGSTOP / SIGCONT ---\n");

    int child_pid = sys_fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        /* Child: infinite loop printing heartbeat */
        int i = 0;
        while (1) {
            printf("tick %d\n", i++);
            sys_sleep(100);
        }
    } else {
        /* Parent */
        sys_sleep(250);

        printf("[parent sends STOP]\n");
        int res = sys_kill(child_pid, SIGNAL_STOP);
        assert(res == 0);

        sys_sleep(600);

        printf("[parent sends CONT]\n");
        res = sys_kill(child_pid, SIGNAL_CONT);
        assert(res == 0);

        sys_sleep(250);

        printf("[parent sends KILL]\n");
        res = sys_kill(child_pid, SIGNAL_KILL);
        assert(res == 0);

        int status = -1;
        res = sys_waitpid(child_pid, &status, 0);
        assert(res == child_pid);
        assert(status == 137);

        printf("Observable SIGSTOP / SIGCONT test passed!\n\n");
    }
}

static void test_kill_stopped_task(void)
{
    printf("--- Test: Kill-a-stopped-task ---\n");

    int child_pid = sys_fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        while (1) {
            sys_sleep(50);
        }
    } else {
        sys_sleep(100);

        printf("[parent] sending STOP to child...\n");
        int res = sys_kill(child_pid, SIGNAL_STOP);
        assert(res == 0);

        sys_sleep(200);

        printf("[parent] sending KILL directly to stopped child (no CONT)...\n");
        res = sys_kill(child_pid, SIGNAL_KILL);
        assert(res == 0);

        int status = -1;
        res = sys_waitpid(child_pid, &status, 0);
        assert(res == child_pid);
        assert(status == 137);

        printf("Kill-a-stopped-task regression test passed! (reaped status 137)\n\n");
    }
}

static void test_cont_racing_stop(void)
{
    printf("--- Test: CONT-racing-STOP ---\n");

    int fds[2];
    int res = sys_pipe(fds);
    assert(res == 0);

    int child_pid = sys_fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        sys_close(fds[0]);
        while (1) {
            sys_write(fds[1], "x", 1);
            sys_sleep(50);
        }
    } else {
        sys_close(fds[1]);
        sys_sleep(100);

        /* Set non-blocking on pipe read end and drain any initial ticks */
        int flags = sys_fcntl(fds[0], VFS_F_GETFL, 0);
        sys_fcntl(fds[0], VFS_F_SETFL, flags | VFS_O_NONBLOCK);
        char dummy[64];
        while (sys_read(fds[0], dummy, sizeof(dummy)) > 0) {}

        printf("[parent] sending STOP immediately followed by CONT (tight window)...\n");
        res = sys_kill(child_pid, SIGNAL_STOP);
        assert(res == 0);
        res = sys_kill(child_pid, SIGNAL_CONT);
        assert(res == 0);

        /* Wait a stretch to verify child continues ticking */
        sys_sleep(300);

        char buf[64];
        int bytes = sys_read(fds[0], buf, sizeof(buf));
        assert(bytes > 0);
        printf("[parent] verified child is still ticking (%d bytes read from pipe)\n", bytes);

        /* Clean up child */
        res = sys_kill(child_pid, SIGNAL_KILL);
        assert(res == 0);

        int status = -1;
        res = sys_waitpid(child_pid, &status, 0);
        assert(res == child_pid);
        assert(status == 137);

        sys_close(fds[0]);
        printf("CONT-racing-STOP regression test passed!\n\n");
    }
}

int main(void)
{
    printf("=== Running SIGSTOP / SIGCONT & Signal Regression Tests ===\n\n");

    test_observable_stop_cont();
    test_kill_stopped_task();
    test_cont_racing_stop();

    printf("=== All SIGSTOP / SIGCONT & Signal Regression Tests Passed! ===\n");
    return 0;
}
