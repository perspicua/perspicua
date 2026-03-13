#include "syscall.h"
#include "string.h"

static void print(const char* s)
{
    sys_write(1, s, strlen(s));
}

int main(void)
{
    print("[INIT  ] Process started\n");

    while (1)
    {
        print("[INIT  ] Forking shell...\n");
        int pid = sys_fork();

        if (pid < 0)
        {
            print("[INIT  ] Error: fork failed\n");
            sys_sleep(1000);
            continue;
        }

        if (pid == 0)
        {
            sys_exec("/sh.elf");
            print("[INIT  ] Error: failed to exec /sh.elf\n");
            sys_exit(1);
        }
        else
        {
            int status = 0;
            sys_waitpid(pid, &status);
            print("[INIT  ] Shell exited, restarting...\n");
            sys_sleep(500);
        }
    }

    return 0;
}
