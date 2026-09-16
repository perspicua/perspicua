/*
 * parse.c - Turning a line of text into a Command.
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

/*
 * expand_variables - Substitutes $?, $$, $NAME and ${NAME} into dst.
 *
 * Expansion is suppressed inside 'single quotes' and honored inside "double
 * quotes" (POSIX). Quote characters are copied through so the later tokenizer
 * can still group words; parse_command strips them. Unknown names expand empty.
 * Note: expanded text is not re-scanned for operators, but IS subject to normal
 * word-splitting when unquoted, which is the usual shell behavior.
 */
void expand_variables(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    int in_single = 0, in_double = 0;

#define SH_PUTC(ch)           \
    do {                      \
        if (j + 1 < dst_size) \
            dst[j++] = (ch);  \
    } while (0)
#define SH_PUTS(str)                            \
    do {                                        \
        for (const char *_s = (str); *_s; _s++) \
            SH_PUTC(*_s);                       \
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
                nlen = sizeof(name) - 1;
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
}

/*
 * expand_operators - Pads redirection and pipe operators with spaces.
 *
 * Lets the tokenizer split on whitespace without breaking quotes. Control
 * operators (; && ||) are handled earlier, so only redirection and pipes
 * reach here.
 */
void expand_operators(const char *line, char *expanded)
{
    int i = 0, j = 0;
    int in_single = 0, in_double = 0;

    while (line[i] != '\0') {
        char c = line[i];
        if (c == '"' && !in_single) {
            in_double = !in_double;
        } else if (c == '\'' && !in_double) {
            in_single = !in_single;
        }

        if (!in_single && !in_double && (c == '<' || c == '>' || c == '|')) {
            if (c == '>' && line[i + 1] == '>') {
                expanded[j++] = ' ';
                expanded[j++] = '>';
                expanded[j++] = '>';
                expanded[j++] = ' ';
                i++;
            } else {
                expanded[j++] = ' ';
                expanded[j++] = c;
                expanded[j++] = ' ';
            }
        } else {
            expanded[j++] = c;
        }
        i++;
    }
    expanded[j] = '\0';
}

void parse_command(char *str, Command *cmd)
{
    cmd->argc = 0;
    cmd->infile = NULL;
    cmd->outfile = NULL;
    cmd->append = 0;
    cmd->background = 0;

    char *tokens[64];
    int token_count = 0;
    char *p = str;

    while (*p && token_count < 64) {
        while (*p == ' ' || *p == '\t') {
            *p++ = '\0';
        }
        if (!*p) {
            break;
        }

        /* Compact one word in place, stripping quotes wherever they appear and
         * keeping quoted whitespace intact (so a"b c"d becomes ab cd, and
         * D='$FOO' becomes D=$FOO). Quotes are only ever removed, so the write
         * cursor never overtakes the read cursor. */
        char *word = p;
        char *w = p;
        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '"' || *p == '\'') {
                char quote = *p++;
                while (*p && *p != quote) {
                    *w++ = *p++;
                }
                if (*p == quote) {
                    p++;
                }
            } else {
                *w++ = *p++;
            }
        }
        /* Consume the delimiter BEFORE terminating the word: when no quotes were
         * removed w == p, so writing '\0' here would otherwise clobber the space
         * and truncate the rest of the line. */
        if (*p) {
            p++;
        }
        *w = '\0';
        tokens[token_count++] = word;
    }

    for (int i = 0; i < token_count; i++) {
        if (strcmp(tokens[i], "<") == 0 && i + 1 < token_count) {
            cmd->infile = tokens[++i];
        } else if (strcmp(tokens[i], ">") == 0 && i + 1 < token_count) {
            cmd->outfile = tokens[++i];
        } else if (strcmp(tokens[i], ">>") == 0 && i + 1 < token_count) {
            cmd->append = 1;
            cmd->outfile = tokens[++i];
        } else if (strcmp(tokens[i], "&") == 0) {
            cmd->background = 1;
        } else {
            if (cmd->argc < MAX_ARGS - 1) {
                cmd->argv[cmd->argc++] = tokens[i];
            }
        }
    }
    cmd->argv[cmd->argc] = NULL;
}
