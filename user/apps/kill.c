#include "syscall.h"
#include "stdlib.h"
#include "stdio.h"
#include "signals.h"
#include "errno.h"

int main(int argc, char **argv)
{
    int sig = SIGNAL_TERM;
    int start = 1;

    /* Optional leading -SIGNUM selects the signal (e.g. kill -9 3). */
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] != '\0') {
        sig = atoi(argv[1] + 1);
        start = 2;
    }

    if (start >= argc) {
        printf("usage: kill [-SIGNUM] PID...\n");
        return 1;
    }

    int rc = 0;
    for (int i = start; i < argc; i++) {
        int pid = atoi(argv[i]);
        if (sys_kill(pid, sig) < 0) {
            if (errno == EPERM) {
                printf("kill: (%d) - operation not permitted\n", pid);
            } else {
                printf("kill: (%d) - no such process\n", pid);
            }
            rc = 1;
        }
    }
    return rc;
}
