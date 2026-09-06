#include "syscall.h"
#include "string.h"
#include "stdio.h"

static int opt_n, opt_i;

/* Buffered line reader over a file descriptor, so lines may span read chunks. */
typedef struct {
    int fd;
    char buf[4096];
    int pos, len;
} Reader;

static int reader_getc(Reader *r)
{
    if (r->pos >= r->len) {
        r->len = sys_read(r->fd, r->buf, sizeof(r->buf));
        r->pos = 0;
        if (r->len <= 0)
            return -1;
    }
    return (unsigned char)r->buf[r->pos++];
}

/* Reads one line (without the newline) into out. Returns 0 at end of input. */
static int read_line(Reader *r, char *out, int max)
{
    int i = 0, c;
    while ((c = reader_getc(r)) != -1) {
        if (c == '\n') {
            out[i] = '\0';
            return 1;
        }
        if (i < max - 1)
            out[i++] = (char)c;
    }
    out[i] = '\0';
    return i > 0;
}

static void to_lower(char *s)
{
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z')
            *s += 32;
    }
}

static int line_matches(const char *line, const char *pattern)
{
    if (!opt_i) {
        return strstr(line, pattern) != NULL;
    }
    char lb[1024], lp[256];
    strncpy(lb, line, sizeof(lb) - 1);
    lb[sizeof(lb) - 1] = '\0';
    strncpy(lp, pattern, sizeof(lp) - 1);
    lp[sizeof(lp) - 1] = '\0';
    to_lower(lb);
    to_lower(lp);
    return strstr(lb, lp) != NULL;
}

static int grep_fd(int fd, const char *pattern, const char *fname)
{
    Reader r = {.fd = fd, .pos = 0, .len = 0};
    char line[1024];
    int lineno = 0, any = 0;
    while (read_line(&r, line, sizeof(line))) {
        lineno++;
        if (line_matches(line, pattern)) {
            any = 1;
            if (fname)
                printf("%s:", fname);
            if (opt_n)
                printf("%d:", lineno);
            printf("%s\n", line);
        }
    }
    return any;
}

int main(int argc, char **argv)
{
    int start = 1;
    for (; start < argc && argv[start][0] == '-' && argv[start][1] != '\0'; start++) {
        for (int j = 1; argv[start][j]; j++) {
            if (argv[start][j] == 'n')
                opt_n = 1;
            else if (argv[start][j] == 'i')
                opt_i = 1;
        }
    }

    if (start >= argc) {
        printf("usage: grep [-ni] PATTERN [FILE...]\n");
        return 2;
    }

    const char *pattern = argv[start++];
    int nfiles = argc - start;
    int found = 0;

    if (nfiles == 0) {
        found |= grep_fd(0, pattern, NULL);
    } else {
        for (int i = start; i < argc; i++) {
            int fd = sys_open(argv[i], VFS_O_RDONLY);
            if (fd < 0) {
                printf("grep: %s: no such file\n", argv[i]);
                continue;
            }
            found |= grep_fd(fd, pattern, nfiles > 1 ? argv[i] : NULL);
            sys_close(fd);
        }
    }

    return found ? 0 : 1;
}
