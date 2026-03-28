#include "syscall.h"
#include "stdio.h"

int main(void)
{
    printf("Reproduction: starting rapid fork/exit...\n");
    for (int i = 0; i < 50; i++) {
        int pid = sys_fork();
        if (pid == 0) {
            sys_exit(0);
        } else if (pid > 0) {
            // Parent doesn't wait immediately to create zombies
        }
    }
    printf("Zombies created. Now reaping...\n");
    while (sys_waitpid(-1, NULL, 0) > 0)
        ;
    printf("Done.\n");
    return 0;
}
