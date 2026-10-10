/*
 * tail - print the last lines of each file.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *slurp(int fd, size_t *len)
{
    size_t cap = 4096;
    char *buf = malloc(cap);
    *len = 0;
    if (!buf) {
        return NULL;
    }
    for (;;) {
        if (*len == cap) {
            char *bigger = realloc(buf, cap * 2);
            if (!bigger) {
                free(buf);
                return NULL;
            }
            buf = bigger;
            cap *= 2;
        }
        int n = read(fd, buf + *len, cap - *len);
        if (n < 0) {
            free(buf);
            return NULL;
        }
        if (n == 0) {
            return buf;
        }
        *len += (size_t)n;
    }
}

static int tail_fd(int fd, long lines)
{
    size_t len;
    char *buf = slurp(fd, &len);
    if (!buf) {
        return -1;
    }

    // A final newline ends the last line rather than starting an empty one.
    size_t i = len > 0 && buf[len - 1] == '\n' ? len - 1 : len;
    long seen = 0;
    if (lines == 0) {
        i = len;
    }
    while (lines > 0 && i > 0) {
        if (buf[i - 1] == '\n' && ++seen == lines) {
            break;
        }
        i--;
    }
    fwrite(buf + i, 1, len - i, stdout);
    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
    long lines = 10;
    int i = 1;

    if (i < argc && strncmp(argv[i], "-n", 2) == 0) {
        const char *num = argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : "");
        lines = atol(num);
        if (lines < 0 || (*num < '0' || *num > '9')) {
            fprintf(stderr, "tail: invalid line count '%s'\n", num);
            return 2;
        }
        i++;
    }

    if (i == argc) {
        return tail_fd(STDIN_FILENO, lines) < 0 ? 1 : 0;
    }

    int status = 0;
    int files = argc - i;
    for (int first = i; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "tail: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        if (files > 1) {
            printf("%s==> %s <==\n", i > first ? "\n" : "", argv[i]);
        }
        if (tail_fd(fd, lines) < 0) {
            fprintf(stderr, "tail: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
        close(fd);
    }
    return status;
}
