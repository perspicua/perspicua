/*
 * sh.h - What the shell's three halves share.
 *
 * The shell is split by what a line goes through: edit.c turns keystrokes into
 * a line, parse.c turns that line into commands, exec.c runs them. Only the
 * handful of names below cross those boundaries.
 */

#ifndef PERSPICUA_SH_H
#define PERSPICUA_SH_H

#include <stdbool.h>
#include <stddef.h>

#define MAX_ARGS    32
#define MAX_REDIRS  8
#define CMD_MAX_LEN 512
#define MAX_CMDS    16
#define EXPAND_MAX  4096 // a line after $-expansion

// One redirection: fd opened on path with flags, or, with no path, fd made a copy of dup_from.
struct redir {
    int fd;
    int flags;
    const char *path;
    int dup_from;
};

// One command in a pipeline: its arguments and its redirections, applied in order.
typedef struct {
    char *argv[MAX_ARGS];
    int argc;
    struct redir redirs[MAX_REDIRS];
    int nredirs;
} Command;

// main.c
char *sh_strdup(const char *s);
char *sh_strndup(const char *s, size_t len);

// edit.c
#define KEY_EOF (-1) // end of input, or a read error that will not go away
int read_key(void);
int sh_read_line(char *buf, size_t size);
void redraw_line(const char *cmd);
void add_to_history(const char *line);
void print_prompt(void);
int do_completions(char *cmd_buffer, int *cmd_len, int cursor_pos);
void clear_completion_matches(void);

// parse.c
// Each returns -1, having said why on stderr, when the line cannot run as written.
void strip_comment(char *line);
int expand_variables(const char *src, char *dst, size_t dst_size);
int expand_operators(const char *line, char *expanded, size_t size);
int parse_command(char *str, Command *cmd);

// exec.c
extern int g_last_status;
extern bool g_job_control;
void execute_line(char *line);
void handle_sigchld(int sig);

#endif // PERSPICUA_SH_H
