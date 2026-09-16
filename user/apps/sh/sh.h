/*
 * sh.h - What the shell's three halves share.
 *
 * The shell is split by what a line goes through: edit.c turns keystrokes into
 * a line, parse.c turns that line into commands, exec.c runs them. Only the
 * handful of names below cross those boundaries.
 */

#ifndef PERSPICUA_SH_H
#define PERSPICUA_SH_H

#include <stddef.h>

#define MAX_ARGS    32
#define CMD_MAX_LEN 512
#define MAX_CMDS    16

/*
 * One command in a pipeline: its arguments, its redirections, and whether the
 * pipeline it belongs to was backgrounded.
 */
typedef struct {
    char *argv[MAX_ARGS];
    int argc;
    char *infile;
    char *outfile;
    int append;
    int background;
} Command;

// main.c
char *sh_strdup(const char *s);
char *sh_strndup(const char *s, size_t len);

// edit.c
int read_key(void);
int sh_read_line(char *buf, size_t size);
void redraw_line(const char *cmd);
void add_to_history(const char *line);
void print_prompt(void);
int do_completions(char *cmd_buffer, int *cmd_len, int cursor_pos);
void clear_completion_matches(void);

// parse.c
void strip_comment(char *line);
void expand_variables(const char *src, char *dst, size_t dst_size);
void expand_operators(const char *line, char *expanded);
void parse_command(char *str, Command *cmd);

// exec.c
extern int g_last_status;
void execute_line(char *line);
void handle_sigchld(int sig);

#endif // PERSPICUA_SH_H
