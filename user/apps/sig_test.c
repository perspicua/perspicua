#include "stdio.h"
#include "syscall.h"
#include "signals.h"

void handle_sigint(int sig)
{
    printf("Caught signal %d!\n", sig);
}

int main()
{
    printf("Signal test starting...\n");

    if (sys_signal(SIGNAL_INT, handle_sigint) < 0) {
        printf("Failed to set signal handler\n");
        return 1;
    }

    printf("Sending SIGNAL_INT to ourselves (PID %d)...\n", sys_getpid());
    sys_kill(sys_getpid(), SIGNAL_INT);

    sys_yield();

    printf("Signal test finished.\n");
    return 0;
}
