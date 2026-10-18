/*
 * exec.c - Running what parse.c produced: builtins, pipelines and job control.
 */

#include "sh.h"

#include <stdbool.h>
#include <stddef.h>

#include "dirent.h"
#include "errno.h"
#include "fcntl.h"
#include "signal.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "syscall.h"
#include "wait.h"

// $?, which && and || test; a signal death is 128 + signo, as sh reports it.
int g_last_status = 0;

// Off for sh -c: jobs then stay in the shell's group and the terminal is never handed over.
bool g_job_control = true;

#define MAX_JOBS 8

// A stopped job: its group, and the members not yet reaped when it stopped.
struct job {
    int pgid;
    int pids[MAX_CMDS];
    int count;
};

// Most recent last; fg resumes from the end.
static struct job stopped_jobs[MAX_JOBS];
static int stopped_count = 0;

static void remember_stopped(int pgid, const int *pids, int count)
{
    if (stopped_count == MAX_JOBS) {
        fprintf(stderr, "sh: too many stopped jobs; group %d is not tracked\n", pgid);
        return;
    }
    struct job *job = &stopped_jobs[stopped_count++];
    job->pgid = pgid;
    job->count = count;
    memcpy(job->pids, pids, sizeof(int) * (size_t)count);
}

void handle_sigchld(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

static int is_parent_builtin(const char *name)
{
    return (strcmp(name, "cd") == 0 || strcmp(name, "exit") == 0 || strcmp(name, "export") == 0
            || strcmp(name, "unset") == 0 || strcmp(name, "fg") == 0 || strcmp(name, "true") == 0
            || strcmp(name, "false") == 0 || strcmp(name, ":") == 0);
}

// Until the job exits or stops, a stopped one parked as a job; reaps background children too.
static int wait_foreground(int pgid, const int *pids, int count)
{
    int shell_pgid = getpgid(0);
    int last_status = 0;

    // handle_sigchld's waitpid(-1) would take a job member's status before the wait below.
    sigset_t chld = 1u << (SIGCHLD - 1);
    sigset_t old_mask;
    sigprocmask(SIG_BLOCK, &chld, &old_mask);

    if (g_job_control) {
        tcsetpgrp(0, pgid);
    }

    // A member that died while its job sat stopped was reaped at the prompt and is no child now.
    int live[MAX_CMDS];
    int n = 0;
    for (int i = 0; i < count; i++) {
        int status = 0;
        int r = waitpid(pids[i], &status, WNOHANG);
        if (r == 0) {
            live[n++] = pids[i];
        } else if (r > 0 && i == count - 1) {
            last_status = WEXITSTATUS(status);
        }
    }

    while (n > 0) {
        int status = 0;
        int r = waitpid(-1, &status, WUNTRACED);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        int m = 0;
        while (m < n && live[m] != r) {
            m++;
        }
        if (m == n) {
            continue;
        }
        // The stop keeps every member not yet reaped for fg.
        if (WIFSTOPPED(status)) {
            remember_stopped(pgid, live, n);
            printf("\n[stopped]  use 'fg' to resume\n");
            last_status = 128 + WSTOPSIG(status);
            break;
        }
        // A pipeline's status is that of its last (rightmost) command.
        if (r == pids[count - 1]) {
            last_status = WEXITSTATUS(status);
        }
        // In pipeline order, so the rightmost member stays last for fg.
        memmove(&live[m], &live[m + 1], sizeof(int) * (size_t)(n - m - 1));
        n--;
    }

    if (g_job_control) {
        tcsetpgrp(0, shell_pgid);
    }
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
        if (stopped_count == 0) {
            fprintf(stderr, "sh: fg: no stopped job\n");
            g_last_status = 1;
        } else {
            struct job job = stopped_jobs[--stopped_count];
            if (kill(-job.pgid, SIGCONT) < 0) {
                fprintf(stderr, "sh: fg: job %d has ended\n", job.pgid);
                g_last_status = 1;
            } else {
                g_last_status = wait_foreground(job.pgid, job.pids, job.count);
            }
        }
    } else if (strcmp(cmd->argv[0], "cd") == 0) {
        const char *target = (cmd->argc > 1) ? cmd->argv[1] : "/";
        if (chdir(target) < 0) {
            fprintf(stderr, "sh: cd: %s: %s\n", target, strerror(errno));
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
        // Home, clear the screen, clear the scrollback
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
        printf("Operators: | < > >> 2> 2>> 2>&1 & ; && ||\n");
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
    char *name = cmd->argv[0];

    // exec keeps SIG_IGN, and a command must stop, die and wait for the terminal as usual.
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);

    // A name containing a slash is a path: exec it directly, never via PATH.
    if (strchr(name, '/')) {
        execve(name, cmd->argv, environ);
    } else {
        execvp(name, cmd->argv);
    }

    // 127 when there is nothing by that name, 126 when there is and it will not run.
    if (errno == ENOENT || errno == ENOTDIR) {
        if (strchr(name, '/')) {
            fprintf(stderr, "sh: %s: %s\n", name, strerror(errno));
        } else {
            fprintf(stderr, "sh: command not found: %s\n", name);
        }
        _exit(127);
    }
    fprintf(stderr, "sh: %s: %s\n", name, strerror(errno));
    _exit(126);
}

// In order, so 2>&1 copies wherever fd 1 points by then.
static int apply_redirections(Command *cmd)
{
    for (int i = 0; i < cmd->nredirs; i++) {
        const struct redir *r = &cmd->redirs[i];
        if (r->path) {
            int fd = open(r->path, r->flags);
            if (fd < 0) {
                fprintf(stderr, "sh: %s: %s\n", r->path, strerror(errno));
                return -1;
            }
            if (fd != r->fd) {
                dup2(fd, r->fd);
                close(fd);
            }
        } else if (dup2(r->dup_from, r->fd) < 0) {
            fprintf(stderr, "sh: %d: %s\n", r->dup_from, strerror(errno));
            return -1;
        }
    }
    return 0;
}

static void execute_pipeline(char *pipe_string, int background)
{
    char *commands_str[MAX_CMDS];
    int num_cmds = 0;

    // Split on unquoted pipes: a quoted one is part of an argument.
    commands_str[num_cmds++] = pipe_string;
    int in_single = 0, in_double = 0;
    for (char *p = pipe_string; *p; p++) {
        if (*p == '\'' && !in_double) {
            in_single = !in_single;
        } else if (*p == '"' && !in_single) {
            in_double = !in_double;
        } else if (*p == '|' && !in_single && !in_double) {
            if (num_cmds == MAX_CMDS) {
                fprintf(stderr, "sh: more than %d commands in a pipeline\n", MAX_CMDS);
                g_last_status = 2;
                return;
            }
            *p = '\0';
            commands_str[num_cmds++] = p + 1;
        }
    }

    static Command cmds[MAX_CMDS];
    for (int i = 0; i < num_cmds; i++) {
        if (parse_command(commands_str[i], &cmds[i]) < 0) {
            g_last_status = 2;
            return;
        }
        if (cmds[i].argc == 0) {
            if (num_cmds > 1) {
                fprintf(stderr, "sh: syntax error: empty command in a pipeline\n");
                g_last_status = 2;
            }
            return;
        }
    }

    if (num_cmds == 1 && !background && is_parent_builtin(cmds[0].argv[0])) {
        run_parent_builtin(&cmds[0]);
        return;
    }

    // From the first fork to the wait, or handle_sigchld reaps a stage that exits early.
    sigset_t chld = 1u << (SIGCHLD - 1);
    sigset_t old_mask;
    sigprocmask(SIG_BLOCK, &chld, &old_mask);

    // A child inherits unflushed parent output and would write it a second time.
    fflush(stdout);

    int pids[MAX_CMDS];
    int started = 0;
    int pgid = 0;
    int prev_pipe = -1;
    int failed = 0;

    for (int i = 0; i < num_cmds; i++) {
        int pipefd[2] = {-1, -1};
        if (i < num_cmds - 1 && pipe(pipefd) < 0) {
            fprintf(stderr, "sh: pipe: %s\n", strerror(errno));
            failed = 1;
            break;
        }

        int pid = fork();
        if (pid < 0) {
            fprintf(stderr, "sh: fork: %s\n", strerror(errno));
            if (pipefd[0] >= 0) {
                close(pipefd[0]);
                close(pipefd[1]);
            }
            failed = 1;
            break;
        }

        if (pid == 0) {
            sigprocmask(SIG_SETMASK, &old_mask, NULL);
            // The child takes the terminal itself, so it never reads before the hand-over.
            if (g_job_control) {
                setpgid(0, pgid);
                if (!background) {
                    tcsetpgrp(0, getpgid(0));
                }
            }

            if (prev_pipe != -1) {
                dup2(prev_pipe, 0);
                close(prev_pipe);
            }
            if (pipefd[1] >= 0) {
                dup2(pipefd[1], 1);
                close(pipefd[0]);
                close(pipefd[1]);
            }

            if (apply_redirections(&cmds[i]) < 0) {
                _exit(1);
            }
            if (is_output_builtin(cmds[i].argv[0])) {
                run_output_builtin(&cmds[i]);
                fflush(stdout);
                _exit(0);
            }
            // In a pipeline or the background, true, cd and the rest run in a subshell (POSIX).
            if (is_parent_builtin(cmds[i].argv[0])) {
                run_parent_builtin(&cmds[i]);
                fflush(stdout);
                _exit(g_last_status);
            }
            run_exec(&cmds[i]);
            _exit(1);
        }

        if (i == 0) {
            pgid = pid;
        }
        if (g_job_control) {
            setpgid(pid, pgid);
        }
        pids[started++] = pid;

        if (prev_pipe != -1) {
            close(prev_pipe);
        }
        prev_pipe = pipefd[0];
        if (pipefd[1] >= 0) {
            close(pipefd[1]);
        }
    }

    if (prev_pipe != -1) {
        close(prev_pipe);
    }

    if (failed) {
        // A half-built pipeline is taken down and reaped, not left running.
        for (int i = 0; i < started; i++) {
            kill(pids[i], SIGKILL);
        }
        for (int i = 0; i < started; i++) {
            waitpid(pids[i], NULL, 0);
        }
        g_last_status = 1;
    } else if (background) {
        g_last_status = 0; // a launched background job "succeeds"
    } else {
        g_last_status = wait_foreground(pgid, pids, started);
    }

    sigprocmask(SIG_SETMASK, &old_mask, NULL);
}

// Expands per command, so $? and $VAR see earlier commands on the line: `false; echo $?`.
static void run_pipeline_segment(char *cmd, int background)
{
    char *s = cmd;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == '\0') {
        return;
    }

    char *vexp = malloc(EXPAND_MAX);
    char *expanded = malloc(EXPAND_MAX);
    if (!vexp || !expanded) {
        fprintf(stderr, "sh: memory allocation failed\n");
        g_last_status = 1;
    } else if (expand_variables(cmd, vexp, EXPAND_MAX) < 0
               || expand_operators(vexp, expanded, EXPAND_MAX) < 0) {
        g_last_status = 2;
    } else {
        execute_pipeline(expanded, background);
    }
    free(vexp);
    free(expanded);
}

enum {
    CONN_ALWAYS,
    CONN_AND,
    CONN_OR
};

// Runs each command whose connector the status so far allows.
static void run_links(char **cmds, const int *conn, int n, int background)
{
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
            run_pipeline_segment(cmds[i], background);
        }
    }
}

// && and || left to right on $?; a skipped link keeps the status, so `false && a || b` runs b.
static void run_conditional_list(char *segment, int background)
{
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
            if (n == MAX_CMDS) {
                fprintf(stderr, "sh: more than %d commands joined by && and ||\n", MAX_CMDS);
                g_last_status = 2;
                return;
            }
            conn[n] = (*p == '&') ? CONN_AND : CONN_OR;
            *p = '\0';
            cmds[n] = p + 2;
            n++;
            p++; // skip the second operator character
        }
    }

    // A backgrounded && / || list runs in a subshell, so its links still wait on each other.
    if (background && n > 1) {
        fflush(stdout);
        int pid = fork();
        if (pid < 0) {
            fprintf(stderr, "sh: fork: %s\n", strerror(errno));
            g_last_status = 1;
            return;
        }
        if (pid == 0) {
            if (g_job_control) {
                setpgid(0, 0);
            }
            g_job_control = false;
            run_links(cmds, conn, n, 0);
            fflush(stdout);
            _exit(g_last_status);
        }
        if (g_job_control) {
            setpgid(pid, pid);
        }
        g_last_status = 0;
        return;
    }
    run_links(cmds, conn, n, background);
}

#define MAX_LISTS 16

void execute_line(char *line)
{
    // A copy, since splitting writes into it, as long as the line itself.
    char *work = sh_strdup(line);
    if (!work) {
        fprintf(stderr, "sh: memory allocation failed\n");
        g_last_status = 1;
        return;
    }

    // Splitting happens before expansion, so expanded text can neither add nor hide an operator.
    strip_comment(work);

    // Split on ';' and a lone '&' (quote-aware); '&' runs the list before it in the background.
    char *lists[MAX_LISTS];
    int background[MAX_LISTS];
    int num_lists = 0;
    lists[num_lists] = work;
    background[num_lists++] = 0;

    int in_single = 0, in_double = 0;
    for (char *p = work; *p; p++) {
        if (*p == '\'' && !in_double) {
            in_single = !in_single;
        } else if (*p == '"' && !in_single) {
            in_double = !in_double;
        } else if (!in_single && !in_double && (*p == ';' || *p == '&')) {
            if (*p == '&' && p[1] == '&') {
                p++;
                continue;
            }
            // The & of >&N names a descriptor.
            if (*p == '&' && p > work && (p[-1] == '>' || p[-1] == '<')) {
                continue;
            }
            if (num_lists == MAX_LISTS) {
                fprintf(stderr, "sh: more than %d commands separated by ; and &\n", MAX_LISTS);
                g_last_status = 2;
                free(work);
                return;
            }
            background[num_lists - 1] = *p == '&';
            *p = '\0';
            lists[num_lists] = p + 1;
            background[num_lists++] = 0;
        }
    }

    for (int i = 0; i < num_lists; i++) {
        run_conditional_list(lists[i], background[i]);
    }

    free(work);
}
