#include "syscall.h"
#include "string.h"
#include "wait.h"

static void print_string(const char* s)
{
    sys_write(1, s, strlen(s));
}

int main(void)
{
    print_string("[ INIT ] Userspace started\n");

    while (1)
    {
        print_string("[ INIT ] Forking shell...\n");
        int shell_pid = sys_fork();

        if (shell_pid < 0)
        {
            print_string("[ INIT ] Error: fork failed\n");
            sys_sleep(1000);
            continue;
        }

        if (shell_pid == 0)
        {
            /* Child process: execute the shell */
            char* argv[] = {"/sh.elf", NULL};
            sys_exec("/sh.elf", argv);
            print_string("[ INIT ] Error: failed to exec /sh.elf\n");
            sys_exit(1);
        }
        else
        {
            /* Parent process: wait for the shell to terminate,
               but also reap any orphaned zombies that get reparented to us. */
            while (1)
            {
                int status = 0;
                int reaped = sys_waitpid(-1, &status, 0);
                if (reaped == shell_pid)
                {
                    /* The shell itself exited */
                    break;
                }
                else if (reaped > 0)
                {
                    /* We reaped an orphan! Just continue waiting. */
                    continue;
                }
                else
                {
                    /* Error or no more children (shouldn't happen as we have shell_pid) */
                    break;
                }
            }
            print_string("[ INIT ] Shell exited, restarting...\n");
            sys_sleep(500);
        }
    }

    return 0;
}
