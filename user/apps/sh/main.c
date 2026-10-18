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
#include "sys/stat.h"
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

static bool stdin_is_terminal(void)
{
    struct stat st;
    return fstat(0, &st) == 0 && S_ISCHR(st.st_mode);
}

// One line of a script, without its newline: 1 read, 0 too long to hold, -1 once input is used up.
static int read_script_line(char *buf, size_t size)
{
    size_t n = 0;
    int got = -1;
    char c;
    while (read(0, &c, 1) == 1) {
        got = got < 0 ? 1 : got;
        if (c == '\n') {
            break;
        }
        if (n < size - 1) {
            buf[n++] = c;
        } else {
            got = 0;
        }
    }
    buf[n] = '\0';
    return got;
}

// Runs lines without job control, as sh -c and a piped script do; exits with the last status.
static int run_noninteractive(char *line)
{
    g_job_control = false;
    if (line) {
        execute_line(line);
        return g_last_status;
    }

    char *buf = malloc(CMD_MAX_LEN);
    if (!buf) {
        fprintf(stderr, "sh: memory allocation failed\n");
        return 1;
    }
    int got;
    while ((got = read_script_line(buf, CMD_MAX_LEN)) >= 0) {
        if (got == 0) {
            fprintf(stderr, "sh: line longer than %d bytes\n", CMD_MAX_LEN - 1);
            g_last_status = 2;
            continue;
        }
        execute_line(buf);
    }
    free(buf);
    return g_last_status;
}

// Job-control signals are for the shell's jobs: a stopped shell leaves nobody reading the console.
static void take_terminal(void)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    // Started in the background, the shell waits to be brought forward before it reads.
    int fg;
    while ((fg = tcgetpgrp(0)) > 0 && fg != getpgid(0)) {
        kill(-getpgid(0), SIGTTIN);
    }
    signal(SIGTTIN, SIG_IGN);

    // A session leader already leads its group; init makes the login shell one.
    setpgid(0, 0);
    tcsetpgrp(0, getpgid(0));
}

int main(int argc, char *argv[], char *envp[])
{
    (void)envp;
    signal(SIGCHLD, handle_sigchld);

    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        return run_noninteractive(argv[2]);
    }
    if (!stdin_is_terminal()) {
        return run_noninteractive(NULL);
    }

    take_terminal();

    printf("Perspicua Shell\n");
    printf("Type help to see available commands.\n\n");

    char *cmd_buffer = malloc(CMD_MAX_LEN);
    if (!cmd_buffer) {
        fprintf(stderr, "sh: memory allocation failed\n");
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
    return g_last_status;
}
