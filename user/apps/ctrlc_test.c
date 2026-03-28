#include "stdio.h"
#include "syscall.h"
#include "signals.h"

void handle_sigint(int sig)
{
    printf("\nCaught Ctrl+C (signal %d)!\n", sig);
    printf("Test PASSED. Exiting...\n");
    sys_exit(0);
}

int main()
{
    printf("Ctrl+C test starting (PID %d)...\n", sys_getpid());
    printf("Press Ctrl+C to test signal delivery.\n");

    if (sys_signal(SIGNAL_INT, handle_sigint) < 0) {
        printf("Failed to set signal handler\n");
        return 1;
    }

    while (1) {
        char c;
        if (sys_read(0, &c, 1) > 0) {
            if (c == '\n')
                printf("\n");
            else
                printf("%c", c);
        }
    }

    return 0;
}
