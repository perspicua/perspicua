/*
 * init.c - PID 1: starts the login shell, restarts it when it exits, and reaps orphans.
 */

#include <stddef.h>

#include "syscall.h"
#include "string.h"
#include "signal.h"
#include "time.h"
#include "wait.h"

static void print_string(const char *s)
{
    write(1, s, strlen(s));
}

static unsigned long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000 + (unsigned long)ts.tv_nsec / 1000000;
}

int main(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    if (getpid() != 1) {
        print_string("[ INIT ] Error: Must run as PID 1\n");
        return 1;
    }
    print_string("[ INIT ] Userspace started\n");

    // Default environment if none provided
    char *default_env[] = {"PATH=/bin:/", "USER=root", "HOME=/", "SHELL=/bin/sh.elf", NULL};
    char **current_env = (envp && envp[0]) ? envp : default_env;

    // A shell that cannot even start waits longer each time, so a card without one is not flooded.
    unsigned int backoff_ms = 500;
    while (1) {
        print_string("[ INIT ] Forking shell...\n");
        unsigned long started = now_ms();
        int shell_pid = fork();

        if (shell_pid < 0) {
            print_string("[ INIT ] Error: fork failed\n");
            usleep((1000) * 1000);
            continue;
        }

        if (shell_pid == 0) {
            // The login session owns the console; the shell's own jobs never start one.
            setsid();
            char *argv[] = {"/bin/sh.elf", NULL};
            execve("/bin/sh.elf", argv, current_env);
            print_string("[ INIT ] Error: failed to exec /bin/sh.elf\n");
            _exit(127);
        }

        int shell_status = 0;
        while (1) {
            int status = 0;
            int reaped = waitpid(-1, &status, WUNTRACED);
            if (reaped == shell_pid && WIFSTOPPED(status)) {
                // A stopped login shell leaves nothing reading the console.
                kill(shell_pid, SIGCONT);
                continue;
            }
            if (reaped == shell_pid) {
                shell_status = status;
                break;
            }
            if (reaped < 0) {
                break;
            }
            // An orphan, reparented to init; reaping it is all there is to do.
        }

        if (WEXITSTATUS(shell_status) == 127 && now_ms() - started < 1000) {
            if (backoff_ms < 30000) {
                backoff_ms = backoff_ms * 2 > 30000 ? 30000 : backoff_ms * 2;
                if (backoff_ms == 30000) {
                    print_string("[ INIT ] The shell will not start; retrying every 30 s\n");
                }
            }
        } else {
            backoff_ms = 500;
            print_string("[ INIT ] Shell exited, restarting...\n");
        }
        usleep(backoff_ms * 1000);
    }

    return 0;
}
