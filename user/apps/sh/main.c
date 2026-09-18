/*
 * main.c - The shell's entry point and its read-eval loop.
 */

#include "sh.h"

#include <stdbool.h>
#include <stddef.h>

#include "dirent.h"
#include "signal.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "syscall.h"
#include "wait.h"

char *sh_strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *new = malloc(len);
    if (new) {
        strcpy(new, s);
    }
    return new;
}

char *sh_strndup(const char *s, size_t len)
{
    char *new = malloc(len + 1);
    if (new) {
        strncpy(new, s, len);
        new[len] = 0;
    }
    return new;
}

int main(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    (void)envp;
    signal(SIGINT, SIG_IGN);
    signal(SIGCHLD, handle_sigchld);
    signal(SIGTTOU, SIG_IGN);

    setsid();
    int shell_pgid = getpid();
    tcsetpgrp(0, shell_pgid);

    printf("Perspicua Shell\n");
    printf("Type help to see available commands.\n\n");

    char *cmd_buffer = malloc(CMD_MAX_LEN);
    if (!cmd_buffer) {
        printf("sh: memory allocation failed\n");
        _exit(1);
    }

    while (1) {
        print_prompt();

        int len = sh_read_line(cmd_buffer, CMD_MAX_LEN);
        if (len < 0) {
            break;
        }
        if (len > 0) {
            add_to_history(cmd_buffer);
            execute_line(cmd_buffer);
        }
    }

    free(cmd_buffer);
    return 0;
}
