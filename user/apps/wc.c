/*
 * wc - print newline, word, and byte counts for each file.
 *
 * Written against POSIX only: the includes below are angle-bracketed and
 * nothing here knows it is running on this kernel. It is the portability
 * check for the libc -- if a stock program stops building, this does too.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct counts {
    unsigned long lines;
    unsigned long words;
    unsigned long bytes;
};

static int show_lines, show_words, show_bytes;

static int count_fd(int fd, struct counts *c)
{
    char buf[4096];
    int in_word = 0;
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        c->bytes += (unsigned long)n;
        for (ssize_t i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\n') {
                c->lines++;
            }
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                c->words++;
            }
        }
    }
    return n < 0 ? -1 : 0;
}

static void report(const struct counts *c, const char *name)
{
    if (show_lines) {
        printf("%8lu", c->lines);
    }
    if (show_words) {
        printf("%8lu", c->words);
    }
    if (show_bytes) {
        printf("%8lu", c->bytes);
    }
    if (name) {
        printf(" %s", name);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    struct counts total = {0, 0, 0};
    int status = 0;
    int i = 1;

    for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        for (const char *p = argv[i] + 1; *p; p++) {
            switch (*p) {
                case 'l':
                    show_lines = 1;
                    break;
                case 'w':
                    show_words = 1;
                    break;
                case 'c':
                    show_bytes = 1;
                    break;
                default:
                    fprintf(stderr, "wc: invalid option -- '%c'\n", *p);
                    return 2;
            }
        }
    }

    if (!show_lines && !show_words && !show_bytes) {
        show_lines = show_words = show_bytes = 1;
    }

    if (i == argc) {
        struct counts c = {0, 0, 0};
        if (count_fd(STDIN_FILENO, &c) < 0) {
            fprintf(stderr, "wc: -: %s\n", strerror(errno));
            return 1;
        }
        report(&c, NULL);
        return 0;
    }

    int files = argc - i;
    for (; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "wc: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }

        struct counts c = {0, 0, 0};
        if (count_fd(fd, &c) < 0) {
            fprintf(stderr, "wc: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        } else {
            report(&c, argv[i]);
            total.lines += c.lines;
            total.words += c.words;
            total.bytes += c.bytes;
        }
        close(fd);
    }

    if (files > 1) {
        report(&total, "total");
    }
    return status;
}
