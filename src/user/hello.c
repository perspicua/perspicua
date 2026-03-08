#include "lib/syscall.h"

int main(void)
{
    int pid = sys_getpid();

    for (int i = 0; i < 5; i++)
    {
        char msg[] = "Hello from separate binary! PID:  \n";
        // Simple int to char for 1-9
        if (pid >= 0 && pid <= 9)
            msg[32] = (char)(pid + '0');
        else
            msg[32] = '?';

        sys_write(msg, sizeof(msg) - 1);
        sys_sleep(500);
    }

    char bye[] = "Process exiting now...\n";
    sys_write(bye, sizeof(bye) - 1);

    sys_exit();
    return 0;
}
