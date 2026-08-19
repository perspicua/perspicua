#include "syscall.h"
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "signals.h"
#include "errno.h"

/* Signal number -> short name, indexed by signal number (0 unused). */
static const char *const signames[] = {
    "0",    "HUP",  "INT",  "QUIT",   "ILL",  "TRAP", "ABRT", "BUS",
    "FPE",  "KILL", "USR1", "SEGV",   "USR2", "PIPE", "ALRM", "TERM",
    "STKFLT", "CHLD", "CONT", "STOP",  "TSTP", "TTIN", "TTOU", "URG",
    "XCPU", "XFSZ", "VTALRM", "PROF", "WINCH", "IO",
};
#define NSIG ((int)(sizeof(signames) / sizeof(signames[0])))

static void list_signals(void)
{
    for (int i = 1; i < NSIG; i++) {
        printf("%2d) SIG%-8s", i, signames[i]);
        if (i % 4 == 0)
            printf("\n");
    }
    if ((NSIG - 1) % 4 != 0)
        printf("\n");
}

int main(int argc, char **argv)
{
    int sig = SIGNAL_TERM;
    int start = 1;

    if (argc > 1 && strcmp(argv[1], "-l") == 0) {
        list_signals();
        return 0;
    }

    /* Optional leading -SIGNUM selects the signal (e.g. kill -9 3). */
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] != '\0') {
        sig = atoi(argv[1] + 1);
        start = 2;
    }

    if (start >= argc) {
        printf("usage: kill [-SIGNUM] PID...   (kill -l lists signals)\n");
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
