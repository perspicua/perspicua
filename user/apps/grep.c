/*
 * grep.c - Prints the lines that contain a fixed string, with -i ignoring case.
 */

#include <stddef.h>

#include "errno.h"
#include "syscall.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

static int opt_n, opt_i;

// Buffered line reader over a file descriptor, so lines may span read chunks.
typedef struct {
    int fd;
    char buf[4096];
    int pos, len;
} Reader;

static int reader_getc(Reader *r)
{
    if (r->pos >= r->len) {
        r->len = read(r->fd, r->buf, sizeof(r->buf));
        r->pos = 0;
        if (r->len <= 0) {
            return -1;
        }
    }
    return (unsigned char)r->buf[r->pos++];
}

// One line without its newline into *line, grown to fit: 1, 0 at end of input, -1 if too long.
static int read_line(Reader *r, char **line, size_t *cap)
{
    size_t n = 0;
    int c, got = 0;
    while ((c = reader_getc(r)) != -1) {
        got = 1;
        if (c == '\n') {
            break;
        }
        if (n + 1 >= *cap) {
            char *bigger = realloc(*line, *cap * 2);
            if (!bigger) {
                return -1;
            }
            *line = bigger;
            *cap *= 2;
        }
        (*line)[n++] = (char)c;
    }
    (*line)[n] = '\0';
    return got;
}

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int contains_nocase(const char *line, const char *pattern)
{
    for (; *line; line++) {
        size_t i = 0;
        while (pattern[i] && lower(line[i]) == lower(pattern[i])) {
            i++;
        }
        if (!pattern[i]) {
            return 1;
        }
    }
    return pattern[0] == '\0';
}

static int line_matches(const char *line, const char *pattern)
{
    return opt_i ? contains_nocase(line, pattern) : strstr(line, pattern) != NULL;
}

// 1 if any line matched, 0 if none did, -1 if the input could not be read through.
static int grep_fd(int fd, const char *pattern, const char *fname)
{
    Reader r = {.fd = fd, .pos = 0, .len = 0};
    size_t cap = 1024;
    char *line = malloc(cap);
    if (!line) {
        fprintf(stderr, "grep: memory allocation failed\n");
        return -1;
    }

    int lineno = 0, any = 0, got;
    while ((got = read_line(&r, &line, &cap)) > 0) {
        lineno++;
        if (line_matches(line, pattern)) {
            any = 1;
            if (fname) {
                printf("%s:", fname);
            }
            if (opt_n) {
                printf("%d:", lineno);
            }
            printf("%s\n", line);
        }
    }
    free(line);

    if (got < 0 || r.len < 0) {
        fprintf(stderr, "grep: %s: %s\n", fname ? fname : "(standard input)",
                got < 0 ? "line too long" : strerror(errno));
        return -1;
    }
    return any;
}

static int usage(void)
{
    fprintf(stderr, "usage: grep [-ni] PATTERN [FILE...]\n");
    return 2;
}

int main(int argc, char **argv)
{
    int start = 1;
    for (; start < argc && argv[start][0] == '-' && argv[start][1] != '\0'; start++) {
        for (int j = 1; argv[start][j]; j++) {
            if (argv[start][j] == 'n') {
                opt_n = 1;
            } else if (argv[start][j] == 'i') {
                opt_i = 1;
            } else {
                fprintf(stderr, "grep: unknown option -%c\n", argv[start][j]);
                return usage();
            }
        }
    }

    if (start >= argc) {
        return usage();
    }

    // POSIX exit status: 0 for a match, 1 for none, 2 when anything could not be read.
    const char *pattern = argv[start++];
    int nfiles = argc - start;
    int found = 0, failed = 0;

    if (nfiles == 0) {
        int r = grep_fd(0, pattern, NULL);
        found |= r > 0;
        failed |= r < 0;
    } else {
        for (int i = start; i < argc; i++) {
            int fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "grep: %s: %s\n", argv[i], strerror(errno));
                failed = 1;
                continue;
            }
            int r = grep_fd(fd, pattern, nfiles > 1 ? argv[i] : NULL);
            found |= r > 0;
            failed |= r < 0;
            close(fd);
        }
    }

    return failed ? 2 : (found ? 0 : 1);
}
