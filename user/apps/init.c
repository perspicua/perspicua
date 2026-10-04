#include <stddef.h>

#include "syscall.h"
#include "string.h"
#include "wait.h"

static void print_string(const char *s)
{
    write(1, s, strlen(s));
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
    char *default_env[] = {"PATH=/bin:/", NULL};
    char **current_env = (envp && envp[0]) ? envp : default_env;

    while (1) {
        print_string("[ INIT ] Forking shell...\n");
        int shell_pid = fork();

        if (shell_pid < 0) {
            print_string("[ INIT ] Error: fork failed\n");
            usleep((1000) * 1000);
            continue;
        }

        if (shell_pid == 0) {
            // Child process: execute the shell
            char *argv[] = {"/bin/sh.elf", NULL};
            execve("/bin/sh.elf", argv, current_env);
            print_string("[ INIT ] Error: failed to exec /bin/sh.elf\n");
            _exit(1);
        } else {
            /* Parent process: wait for the shell to terminate,
               but also reap any orphaned zombies that get reparented to us. */
            while (1) {
                int status = 0;
                int reaped = waitpid(-1, &status, 0);
                if (reaped == shell_pid) {
                    // The shell itself exited
                    break;
                } else if (reaped > 0) {
                    // We reaped an orphan! Just continue waiting.
                    continue;
                } else {
                    // Error or no more children (shouldn't happen as we have shell_pid)
                    break;
                }
            }
            print_string("[ INIT ] Shell exited, restarting...\n");
            usleep((500) * 1000);
        }
    }

    return 0;
}
