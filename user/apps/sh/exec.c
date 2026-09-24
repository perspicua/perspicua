/*
 * exec.c - Running what parse.c produced: builtins, pipelines and job control.
 */

#include "sh.h"

#include <stdbool.h>
#include <stddef.h>

#include "dirent.h"
#include "errno.h"
#include "signal.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "syscall.h"
#include "wait.h"

/*
 * Exit status of the last foreground command, exposed to scripts as $? and used
 * to short-circuit && / || lists. Signals report as 128 + signo, matching sh.
 */
int g_last_status = 0;

static int stopped_pgid = 0;

void handle_sigchld(int sig)
{
    (void)sig;
    // Reap any finished background children
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

static int is_parent_builtin(const char *name)
{
    return (strcmp(name, "cd") == 0 || strcmp(name, "exit") == 0 || strcmp(name, "export") == 0
            || strcmp(name, "unset") == 0 || strcmp(name, "fg") == 0 || strcmp(name, "true") == 0
            || strcmp(name, "false") == 0 || strcmp(name, ":") == 0);
}

/*
 * wait_foreground - Waits on a foreground job, holding the terminal.
 *
 * Returns after the job exits or stops. A stop leaves the job parked and
 * recorded rather than reaped, so the prompt comes back with it still alive.
 */
static int wait_foreground(int pgid, const int *pids, int count)
{
    int shell_pgid = getpgid(0);
    int last_status = 0;

    // handle_sigchld reaps with waitpid(-1), which would take a job member's
    // status before the wait below sees it.
    sigset_t chld = 1u << (SIGCHLD - 1);
    sigset_t old_mask;
    sigprocmask(SIG_BLOCK, &chld, &old_mask);

    tcsetpgrp(0, pgid);
    for (int i = 0; i < count; i++) {
        int status = 0;
        int r;
        do {
            r = waitpid(pids[i], &status, WUNTRACED);
        } while (r < 0 && errno == EINTR);
        if (r < 0) {
            continue;
        }
        if (WIFSTOPPED(status)) {
            stopped_pgid = pgid;
            printf("\n[stopped]  use 'fg' to resume\n");
            last_status = 128 + WSTOPSIG(status);
            break;
        }
        // A pipeline's status is that of its last (rightmost) command.
        if (i == count - 1) {
            last_status = status & 0xFF;
        }
    }
    tcsetpgrp(0, shell_pgid);
    sigprocmask(SIG_SETMASK, &old_mask, NULL);
    return last_status;
}

static void run_parent_builtin(Command *cmd)
{
    if (strcmp(cmd->argv[0], "exit") == 0) {
        // `exit` with no argument exits with the last command's status.
        _exit(cmd->argc > 1 ? atoi(cmd->argv[1]) : g_last_status);
    } else if (strcmp(cmd->argv[0], "true") == 0 || strcmp(cmd->argv[0], ":") == 0) {
        g_last_status = 0;
    } else if (strcmp(cmd->argv[0], "false") == 0) {
        g_last_status = 1;
    } else if (strcmp(cmd->argv[0], "fg") == 0) {
        if (stopped_pgid == 0) {
            printf("sh: fg: no stopped job\n");
            g_last_status = 1;
        } else {
            int pgid = stopped_pgid;
            stopped_pgid = 0;
            kill(-pgid, SIGCONT);
            int pids[1] = {pgid};
            g_last_status = wait_foreground(pgid, pids, 1);
        }
    } else if (strcmp(cmd->argv[0], "cd") == 0) {
        const char *target = (cmd->argc > 1) ? cmd->argv[1] : "/";
        if (chdir(target) < 0) {
            printf("sh: cd: no such directory: %s\n", target);
            g_last_status = 1;
        } else {
            g_last_status = 0;
        }
    } else if (strcmp(cmd->argv[0], "export") == 0) {
        if (cmd->argc > 1) {
            char *arg = cmd->argv[1];
            char *equals = strchr(arg, '=');
            if (equals) {
                *equals = '\0';
                setenv(arg, equals + 1, 1);
                *equals = '='; // Restore if needed, though arg is local to parse_command's tokens
            } else {
                setenv(arg, "", 1);
            }
        }
        g_last_status = 0;
    } else if (strcmp(cmd->argv[0], "unset") == 0) {
        if (cmd->argc > 1) {
            unsetenv(cmd->argv[1]);
        }
        g_last_status = 0;
    }
}

static int is_output_builtin(const char *name)
{
    return (strcmp(name, "clear") == 0 || strcmp(name, "echo") == 0 || strcmp(name, "pwd") == 0
            || strcmp(name, "help") == 0 || strcmp(name, "env") == 0);
}

static void run_output_builtin(Command *cmd)
{
    if (strcmp(cmd->argv[0], "clear") == 0) {
        /* \033[H  - Move cursor to home (1,1)
         * \033[2J - Clear entire screen
         * \033[3J - Clear scrollback buffer
         */
        printf("\033[H\033[2J\033[3J");
    } else if (strcmp(cmd->argv[0], "echo") == 0) {
        for (int i = 1; i < cmd->argc; i++) {
            printf("%s", cmd->argv[i]);
            if (i < cmd->argc - 1) {
                printf(" ");
            }
        }
        printf("\n");
    } else if (strcmp(cmd->argv[0], "pwd") == 0) {
        char cwd[256];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            printf("%s\n", cwd);
        }
    } else if (strcmp(cmd->argv[0], "help") == 0) {
        printf("Perspicua Shell\n");
        printf("Built-ins: help, echo, clear, pwd, cd, exit, export, unset, env, "
               "true, false, :, fg\n");
        printf("Operators: | > >> < & ; && ||\n");
        printf("Scripting: $?  $$  $VAR  ${VAR}  'single'  \"double\"  # comments\n");
    } else if (strcmp(cmd->argv[0], "env") == 0) {
        if (environ) {
            for (int i = 0; environ[i]; i++) {
                printf("%s\n", environ[i]);
            }
        }
    }
}

static void run_exec(Command *cmd)
{
    char path[256];
    char *name = cmd->argv[0];

    /* The shell ignores SIGINT so Ctrl-C doesn't kill it, and exec preserves
     * SIG_IGN. Restore the default so a foreground command responds to Ctrl-C. */
    signal(SIGINT, SIG_DFL);

    // A name containing a slash is a path: exec it directly, never via PATH.
    if (strchr(name, '/')) {
        execve(name, cmd->argv, environ);
        printf("sh: %s : no such file or directory\n", name);
        _exit(127);
    }

    char *path_env = getenv("PATH");
    if (!path_env) {
        path_env = "/bin:/";
    }

    char path_copy[256];
    strncpy(path_copy, path_env, sizeof(path_copy));
    path_copy[sizeof(path_copy) - 1] = '\0';

    char *dir = strtok(path_copy, ":");
    while (dir) {
        // Try dir/name.elf
        strcpy(path, dir);
        int len = strlen(path);
        if (len > 0 && path[len - 1] != '/') {
            strcat(path, "/");
        }
        strcat(path, name);
        strcat(path, ".elf");
        execve(path, cmd->argv, environ);

        // Try dir/name
        strcpy(path, dir);
        len = strlen(path);
        if (len > 0 && path[len - 1] != '/') {
            strcat(path, "/");
        }
        strcat(path, name);
        execve(path, cmd->argv, environ);

        dir = strtok(NULL, ":");
    }

    printf("sh: command not found: %s\n", name);
    _exit(127);
}

static int apply_redirections(Command *cmd)
{
    if (cmd->infile) {
        int fd = open(cmd->infile, O_RDONLY);
        if (fd < 0) {
            printf("sh: cannot open input file\n");
            return -1;
        }
        dup2(fd, 0);
        close(fd);
    }
    if (cmd->outfile) {
        int flags = O_WRONLY | O_CREAT | (cmd->append ? O_APPEND : O_TRUNC);
        int fd = open(cmd->outfile, flags);
        if (fd < 0) {
            printf("sh: cannot open output file\n");
            return -1;
        }
        dup2(fd, 1);
        close(fd);
    }
    return 0;
}

static void execute_pipeline(char *pipe_string)
{
    char *commands_str[MAX_CMDS];
    int num_cmds = 0;

    char *p = pipe_string;
    commands_str[num_cmds++] = p;
    while (*p) {
        if (*p == '|') {
            *p = '\0';
            commands_str[num_cmds++] = p + 1;
        }
        p++;
    }

    if (num_cmds == 1) {
        Command cmd;
        parse_command(commands_str[0], &cmd);
        if (cmd.argc == 0) {
            return;
        }

        if (is_parent_builtin(cmd.argv[0])) {
            run_parent_builtin(&cmd);
            return;
        }

        int pid = fork();
        if (pid == 0) {
            setpgid(0, 0);
            if (apply_redirections(&cmd) < 0) {
                _exit(1);
            }

            if (is_output_builtin(cmd.argv[0])) {
                run_output_builtin(&cmd);
                _exit(0);
            }
            run_exec(&cmd);
            _exit(1);
        } else {
            setpgid(pid, pid);
            if (!cmd.background) {
                int pids[1] = {pid};
                g_last_status = wait_foreground(pid, pids, 1);
            } else {
                g_last_status = 0; // a launched background job "succeeds"
            }
        }
        return;
    }

    // Handle multiple piped commands
    int prev_pipe = -1;
    int pipefd[2];
    int pids[MAX_CMDS];
    int bg_flag = 0;
    int pipeline_pgid = 0;

    for (int i = 0; i < num_cmds; i++) {
        Command cmd;
        parse_command(commands_str[i], &cmd);
        if (cmd.argc == 0) {
            continue;
        }
        if (cmd.background) {
            bg_flag = 1;
        }

        if (i < num_cmds - 1) {
            if (pipe(pipefd) < 0) {
                printf("sh: pipe failed\n");
                return;
            }
        }

        int pid = fork();
        if (pid == 0) {
            if (i == 0) {
                setpgid(0, 0);
            } else {
                setpgid(0, pipeline_pgid);
            }

            if (prev_pipe != -1) {
                dup2(prev_pipe, 0);
                close(prev_pipe);
            }
            if (i < num_cmds - 1) {
                dup2(pipefd[1], 1);
                close(pipefd[0]);
                close(pipefd[1]);
            }

            if (apply_redirections(&cmd) < 0) {
                _exit(1);
            }

            if (is_output_builtin(cmd.argv[0])) {
                run_output_builtin(&cmd);
                _exit(0);
            }
            run_exec(&cmd);
            _exit(1);
        } else {
            pids[i] = pid;
            if (i == 0) {
                pipeline_pgid = pid;
            }
            setpgid(pid, pipeline_pgid);

            if (prev_pipe != -1) {
                close(prev_pipe);
            }
            if (i < num_cmds - 1) {
                close(pipefd[1]);
                prev_pipe = pipefd[0];
            }
        }
    }

    if (!bg_flag && pipeline_pgid > 0) {
        g_last_status = wait_foreground(pipeline_pgid, pids, num_cmds);
    } else if (bg_flag) {
        g_last_status = 0;
    }
}

/* Run one leaf command (already free of ; && ||) through $-expansion, operator
 * spacing, and the pipeline executor. Whitespace-only segments are no-ops.
 *
 * Variable expansion happens HERE, per command, rather than once for the whole
 * line, so that $? and $VAR reflect commands run earlier in the same line
 * (e.g. `false; echo $?` and `export X=1; echo $X`). */
static void run_pipeline_segment(char *cmd)
{
    char *s = cmd;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == '\0') {
        return;
    }

    char *vexp = malloc(CMD_MAX_LEN * 2);
    char *expanded = malloc(CMD_MAX_LEN * 2);
    if (!vexp || !expanded) {
        printf("sh: memory allocation failed\n");
        free(vexp);
        free(expanded);
        return;
    }
    expand_variables(cmd, vexp, CMD_MAX_LEN * 2);
    expand_operators(vexp, expanded);
    execute_pipeline(expanded);
    free(vexp);
    free(expanded);
}

/*
 * run_conditional_list - Executes a ';'-free segment whose commands may be joined
 * by && and ||, with short-circuit evaluation driven by $?.
 *
 * Left-associative: each link runs based only on the immediately preceding
 * connector and the running status. Skipping leaves the status untouched, which
 * makes mixed chains like `false && a || b` behave as sh does (b runs).
 */
static void run_conditional_list(char *segment)
{
    enum {
        CONN_ALWAYS,
        CONN_AND,
        CONN_OR
    };
    char *cmds[MAX_CMDS];
    int conn[MAX_CMDS];
    int n = 0;

    cmds[n] = segment;
    conn[n] = CONN_ALWAYS;
    n++;

    int in_single = 0, in_double = 0;
    for (char *p = segment; *p; p++) {
        if (*p == '\'' && !in_double) {
            in_single = !in_single;
        } else if (*p == '"' && !in_single) {
            in_double = !in_double;
        } else if (!in_single && !in_double && (*p == '&' || *p == '|') && p[1] == *p) {
            if (n >= MAX_CMDS) {
                break;
            }
            conn[n] = (*p == '&') ? CONN_AND : CONN_OR;
            *p = '\0';
            cmds[n] = p + 2;
            n++;
            p++; // skip the second operator character
        }
    }

    for (int i = 0; i < n; i++) {
        int run;
        if (conn[i] == CONN_AND) {
            run = (g_last_status == 0);
        } else if (conn[i] == CONN_OR) {
            run = (g_last_status != 0);
        } else {
            run = 1;
        }

        if (run) {
            run_pipeline_segment(cmds[i]);
        }
    }
}

void execute_line(char *line)
{
    char *work = malloc(CMD_MAX_LEN);
    if (!work) {
        printf("sh: memory allocation failed\n");
        return;
    }

    /* Comments are stripped once, up front. Operator splitting (below) and
     * $-expansion (per command, deeper down) run on the raw text so that
     * expansion cannot inject or hide control operators. */
    strncpy(work, line, CMD_MAX_LEN - 1);
    work[CMD_MAX_LEN - 1] = '\0';
    strip_comment(work);

    // Split by ';' (quote-aware) into sequential lists.
    char *seq_commands[16];
    int num_seq = 0;
    seq_commands[num_seq++] = work;

    int in_single = 0, in_double = 0;
    for (char *p = work; *p; p++) {
        if (*p == '\'' && !in_double) {
            in_single = !in_single;
        } else if (*p == '"' && !in_single) {
            in_double = !in_double;
        } else if (*p == ';' && !in_single && !in_double) {
            *p = '\0';
            if (num_seq < 16) {
                seq_commands[num_seq++] = p + 1;
            }
        }
    }

    for (int i = 0; i < num_seq; i++) {
        run_conditional_list(seq_commands[i]);
    }

    free(work);
}
