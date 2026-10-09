/*
 * watch - rerun a command every few seconds on a cleared screen; Ctrl-C stops it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    long secs = 2;
    int i = 1;
    if (i + 1 < argc && strcmp(argv[i], "-n") == 0) {
        secs = atol(argv[i + 1]);
        i += 2;
    }
    if (i >= argc || secs <= 0) {
        fprintf(stderr, "usage: watch [-n seconds] command [args...]\n");
        return 2;
    }

    for (;;) {
        printf("\033[2J\033[HEvery %lds:", secs);
        for (int k = i; k < argc; k++) {
            printf(" %s", argv[k]);
        }
        printf("\n\n");

        int pid = fork();
        if (pid < 0) {
            fprintf(stderr, "watch: cannot fork\n");
            return 1;
        }
        if (pid == 0) {
            execvp(argv[i], argv + i);
            fprintf(stderr, "watch: %s: command not found\n", argv[i]);
            _exit(127);
        }
        int status;
        waitpid(pid, &status, 0);

        struct timespec req = {.tv_sec = secs};
        nanosleep(&req, NULL);
    }
}
