/*
 * parse.c - Turning a line of text into a Command.
 */

#include "sh.h"

#include <stdbool.h>
#include <stddef.h>

#include "dirent.h"
#include "fcntl.h"
#include "signal.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "syscall.h"
#include "wait.h"

void strip_comment(char *line)
{
    int in_single = 0, in_double = 0;
    for (int i = 0; line[i]; i++) {
        char c = line[i];
        if (c == '\'' && !in_double) {
            in_single = !in_single;
        } else if (c == '"' && !in_single) {
            in_double = !in_double;
        } else if (c == '#' && !in_single && !in_double
                   && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) {
            line[i] = '\0';
            return;
        }
    }
}

// Expanded characters travel as stand-ins until parse_command, so nothing reads them as syntax.
static const char lit_syntax[] = "|<>&\"'";

static char lit_encode(char c)
{
    const char *hit = strchr(lit_syntax, c);
    return (c != '\0' && hit) ? (char)(1 + (hit - lit_syntax)) : c;
}

// In place, once a word's role is decided, so an expanded ">" stays a word.
static char *lit_decode(char *word)
{
    for (char *c = word; *c; c++) {
        if (*c >= 1 && *c <= (char)(sizeof(lit_syntax) - 1)) {
            *c = lit_syntax[*c - 1];
        }
    }
    return word;
}

// $?, $$, $NAME, ${NAME}: not in '', yes in ""; quotes pass through, unset names expand empty.
int expand_variables(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    int in_single = 0, in_double = 0;
    int overflow = 0;

#define SH_PUTC(ch)           \
    do {                      \
        if (j + 1 < dst_size) \
            dst[j++] = (ch);  \
        else                  \
            overflow = 1;     \
    } while (0)
#define SH_PUTS(str)                            \
    do {                                        \
        for (const char *_s = (str); *_s; _s++) \
            SH_PUTC(lit_encode(*_s));           \
    } while (0)

    for (size_t i = 0; src[i]; i++) {
        char c = src[i];

        if (c == '\'' && !in_double) {
            in_single = !in_single;
            SH_PUTC(c);
            continue;
        }
        if (c == '"' && !in_single) {
            in_double = !in_double;
            SH_PUTC(c);
            continue;
        }

        if (c == '$' && !in_single) {
            char next = src[i + 1];
            char num[16];

            if (next == '?') {
                snprintf(num, sizeof(num), "%d", g_last_status);
                SH_PUTS(num);
                i++;
                continue;
            }
            if (next == '$') {
                snprintf(num, sizeof(num), "%d", getpid());
                SH_PUTS(num);
                i++;
                continue;
            }

            // $NAME or ${NAME}
            int braced = (next == '{');
            size_t start = braced ? i + 2 : i + 1;
            size_t end = start;
            while (src[end]
                   && ((src[end] >= 'A' && src[end] <= 'Z') || (src[end] >= 'a' && src[end] <= 'z')
                       || (src[end] >= '0' && src[end] <= '9') || src[end] == '_')) {
                end++;
            }

            if (end == start || (braced && src[end] != '}')) {
                // Not a valid name (e.g. lone '$' or unterminated '${'): literal.
                SH_PUTC(c);
                continue;
            }

            char name[64];
            size_t nlen = end - start;
            if (nlen >= sizeof(name)) {
                fprintf(stderr, "sh: variable name longer than %d bytes\n", (int)sizeof(name) - 1);
                dst[0] = '\0';
                return -1;
            }
            memcpy(name, src + start, nlen);
            name[nlen] = '\0';

            char *val = getenv(name);
            if (val) {
                SH_PUTS(val);
            }
            i = braced ? end : end - 1; // loop ++ advances past '}' or last name char
            continue;
        }

        SH_PUTC(c);
    }

    dst[j] = '\0';
#undef SH_PUTC
#undef SH_PUTS
    if (overflow) {
        fprintf(stderr, "sh: line longer than %d bytes after expansion\n", (int)dst_size - 1);
        return -1;
    }
    return 0;
}

// Pads | and redirections with spaces outside quotes, keeping 2> and >&N on their operator.
int expand_operators(const char *line, char *expanded, size_t size)
{
    size_t i = 0, j = 0;
    int in_single = 0, in_double = 0;

    while (line[i] != '\0' && j + 8 < size) {
        char c = line[i];
        if (c == '"' && !in_single) {
            in_double = !in_double;
        } else if (c == '\'' && !in_double) {
            in_single = !in_single;
        }

        if (!in_single && !in_double && c == '|') {
            expanded[j++] = ' ';
            expanded[j++] = c;
            expanded[j++] = ' ';
        } else if (!in_single && !in_double && (c == '<' || c == '>')) {
            int fd_digit = j > 0 && expanded[j - 1] >= '0' && expanded[j - 1] <= '9'
                           && (j == 1 || expanded[j - 2] == ' ' || expanded[j - 2] == '\t');
            if (!fd_digit) {
                expanded[j++] = ' ';
            }
            expanded[j++] = c;
            if (c == '>' && line[i + 1] == '>') {
                expanded[j++] = line[++i];
            } else if (line[i + 1] == '&' && line[i + 2] >= '0' && line[i + 2] <= '9') {
                expanded[j++] = line[++i];
                expanded[j++] = line[++i];
            }
            expanded[j++] = ' ';
        } else {
            expanded[j++] = c;
        }
        i++;
    }
    expanded[j] = '\0';
    if (line[i] != '\0') {
        fprintf(stderr, "sh: line longer than %d bytes\n", (int)size - 8);
        return -1;
    }
    return 0;
}

// [N]<, [N]>, [N]>>, [N]>&M or [N]<&M: 1 for a file (path next), 2 for a dup, 0 for neither.
static int parse_redir(const char *word, struct redir *r)
{
    const char *p = word;
    int fd = -1;
    if (*p >= '0' && *p <= '9') {
        fd = *p++ - '0';
    }
    char op = *p++;
    if (op != '<' && op != '>') {
        return 0;
    }
    int append = op == '>' && *p == '>';
    if (append) {
        p++;
    }
    r->fd = fd >= 0 ? fd : (op == '<' ? 0 : 1);
    r->path = NULL;
    r->dup_from = -1;
    if (*p == '&' && !append) {
        if (p[1] < '0' || p[1] > '9' || p[2] != '\0') {
            return 0;
        }
        r->dup_from = p[1] - '0';
        return 2;
    }
    if (*p != '\0') {
        return 0;
    }
    r->flags = op == '<' ? O_RDONLY : O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    return 1;
}

int parse_command(char *str, Command *cmd)
{
    cmd->argc = 0;
    cmd->nredirs = 0;
    cmd->argv[0] = NULL;

    char *tokens[MAX_ARGS + 2 * MAX_REDIRS];
    int token_count = 0;
    char *p = str;

    while (*p) {
        while (*p == ' ' || *p == '\t') {
            *p++ = '\0';
        }
        if (!*p) {
            break;
        }
        if (token_count == (int)(sizeof(tokens) / sizeof(tokens[0]))) {
            fprintf(stderr, "sh: more than %d words in a command\n", token_count);
            return -1;
        }

        // Strips quotes in place, a"b c"d to ab cd; removal only, so w never passes p.
        char *word = p;
        char *w = p;
        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '"' || *p == '\'') {
                char quote = *p++;
                while (*p && *p != quote) {
                    *w++ = lit_encode(*p++); // quoted, so never an operator
                }
                if (*p == quote) {
                    p++;
                }
            } else {
                *w++ = *p++;
            }
        }
        // Past the delimiter first: with no quotes removed w == p, and the '\0' would hit it.
        if (*p) {
            p++;
        }
        *w = '\0';
        tokens[token_count++] = word;
    }

    for (int i = 0; i < token_count; i++) {
        struct redir r;
        int kind = parse_redir(tokens[i], &r);
        if (kind == 0) {
            if (cmd->argc == MAX_ARGS - 1) {
                fprintf(stderr, "sh: more than %d arguments\n", MAX_ARGS - 1);
                return -1;
            }
            cmd->argv[cmd->argc++] = lit_decode(tokens[i]);
            continue;
        }
        if (cmd->nredirs == MAX_REDIRS) {
            fprintf(stderr, "sh: more than %d redirections\n", MAX_REDIRS);
            return -1;
        }
        if (kind == 1) {
            if (i + 1 == token_count || parse_redir(tokens[i + 1], &(struct redir){0}) != 0) {
                fprintf(stderr, "sh: syntax error: %s needs a file name\n", tokens[i]);
                return -1;
            }
            r.path = lit_decode(tokens[++i]);
        }
        cmd->redirs[cmd->nredirs++] = r;
    }
    cmd->argv[cmd->argc] = NULL;
    return 0;
}
