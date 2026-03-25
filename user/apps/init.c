
#include "syscall.h"
#include "string.h"
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
        int pid = sys_fork();

        if (pid < 0)
        {
            print_string("[ INIT ] Error: fork failed\n");
            sys_sleep(1000);
            continue;
        }

        if (pid == 0)
        {
            /* Child process: execute the shell */
            sys_exec("/sh.elf");
            print_string("[ INIT ] Error: failed to exec /sh.elf\n");
            sys_exit(1);
        }
        else
        {
            /* Parent process: wait for the shell to terminate */
            int status = 0;
            sys_waitpid(pid, &status, 0);
            print_string("[ INIT ] Shell exited, restarting...\n");
            sys_sleep(500);
        }
    }

    return 0;
}
