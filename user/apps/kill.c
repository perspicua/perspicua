/*
 * kill.c - Sends a signal, by number or name, to processes or process groups.
 */

#include "syscall.h"
#include "string.h"
#include "stdio.h"
#include "signal.h"
#include "errno.h"

// Signal number -> short name, indexed by signal number (0 unused).
static const char *const signames[] = {
    "0",    "HUP",  "INT",  "QUIT", "ILL",    "TRAP",   "ABRT",  "BUS",  "FPE",  "KILL", "USR1",
    "SEGV", "USR2", "PIPE", "ALRM", "TERM",   "STKFLT", "CHLD",  "CONT", "STOP", "TSTP", "TTIN",
    "TTOU", "URG",  "XCPU", "XFSZ", "VTALRM", "PROF",   "WINCH", "IO",   "PWR",  "SYS",
};

// list_signals indexes this by every signal the kernel defines.
_Static_assert(sizeof(signames) / sizeof(signames[0]) == NSIG, "signames is missing a signal");

static void list_signals(void)
{
    for (int i = 1; i < NSIG; i++) {
        printf("%2d) SIG%-8s", i, signames[i]);
        if (i % 4 == 0) {
            printf("\n");
        }
    }
    if ((NSIG - 1) % 4 != 0) {
        printf("\n");
    }
}

// All digits after an optional '-'; writes the value to *out.
static int parse_int(const char *s, int *out)
{
    int neg = (*s == '-');
    if (neg) {
        s++;
    }
    if (*s == '\0') {
        return 0;
    }
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000000) {
            return 0;
        }
        v = v * 10 + (*s - '0');
    }
    *out = neg ? -v : v;
    return 1;
}

// A number, a name, or a name with SIG in front: "9", "KILL" and "SIGKILL" alike. -1 if none.
static int parse_signal(const char *s)
{
    int n;
    if (parse_int(s, &n)) {
        return (n >= 0 && n < NSIG) ? n : -1;
    }
    if (strncmp(s, "SIG", 3) == 0) {
        s += 3;
    }
    for (int i = 1; i < NSIG; i++) {
        if (strcmp(s, signames[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static int usage(void)
{
    fprintf(stderr, "usage: kill [-SIGNAL | -s SIGNAL] PID...   (kill -l lists signals)\n");
    return 1;
}

int main(int argc, char **argv)
{
    int sig = SIGTERM;
    int start = 1;

    if (argc > 1 && strcmp(argv[1], "-l") == 0) {
        list_signals();
        return 0;
    }

    const char *name = NULL;
    if (argc > 2 && strcmp(argv[1], "-s") == 0) {
        name = argv[2];
        start = 3;
    } else if (argc > 1 && argv[1][0] == '-' && argv[1][1] != '\0' && strcmp(argv[1], "--") != 0) {
        name = argv[1] + 1;
        start = 2;
    }
    if (name) {
        sig = parse_signal(name);
        if (sig < 0) {
            fprintf(stderr, "kill: invalid signal '%s'\n", name);
            return 1;
        }
    }
    if (start < argc && strcmp(argv[start], "--") == 0) {
        start++;
    }

    if (start >= argc) {
        return usage();
    }

    int rc = 0;
    for (int i = start; i < argc; i++) {
        // A negative pid names a process group; anything else that is not a number is an error.
        int pid;
        if (!parse_int(argv[i], &pid)) {
            fprintf(stderr, "kill: invalid pid '%s'\n", argv[i]);
            rc = 1;
            continue;
        }
        if (kill(pid, sig) < 0) {
            fprintf(stderr, "kill: (%d) - %s\n", pid, strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
