/*
 * head - print the first lines of each file.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int head_fd(int fd, long lines)
{
    char buf[4096];
    long seen = 0;
    int n = 0;

    while (seen < lines && (n = read(fd, buf, sizeof(buf))) > 0) {
        int upto = 0;
        while (upto < n && seen < lines) {
            if (buf[upto++] == '\n') {
                seen++;
            }
        }
        write(STDOUT_FILENO, buf, upto);
    }
    return n < 0 ? -1 : 0;
}

int main(int argc, char **argv)
{
    long lines = 10;
    int i = 1;

    if (i < argc && strncmp(argv[i], "-n", 2) == 0) {
        const char *num = argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : "");
        lines = atol(num);
        if (lines < 0 || (*num < '0' || *num > '9')) {
            fprintf(stderr, "head: invalid line count '%s'\n", num);
            return 2;
        }
        i++;
    }

    if (i == argc) {
        return head_fd(STDIN_FILENO, lines) < 0 ? 1 : 0;
    }

    int status = 0;
    int files = argc - i;
    for (int first = i; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "head: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        if (files > 1) {
            printf("%s==> %s <==\n", i > first ? "\n" : "", argv[i]);
        }
        if (head_fd(fd, lines) < 0) {
            fprintf(stderr, "head: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
        close(fd);
    }
    return status;
}
