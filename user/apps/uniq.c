/*
 * uniq - collapse runs of identical adjacent lines.
 *
 * -c prefixes each line with how many times it repeated.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int show_count;
static char *prev;
static size_t prev_len, prev_cap;
static int have_prev;
static long repeats;

static void flush_prev(void)
{
    if (!have_prev) {
        return;
    }
    if (show_count) {
        printf("%7ld ", repeats);
    }
    fwrite(prev, 1, prev_len, stdout);
    fwrite("\n", 1, 1, stdout);
}

// Lines compare whole, however long; -1 if there was no memory to keep this one.
static int take_line(const char *line, size_t len)
{
    if (have_prev && len == prev_len && memcmp(line, prev, len) == 0) {
        repeats++;
        return 0;
    }
    flush_prev();
    if (len + 1 > prev_cap) {
        char *bigger = realloc(prev, len + 1);
        if (!bigger) {
            return -1;
        }
        prev = bigger;
        prev_cap = len + 1;
    }
    memcpy(prev, line, len);
    prev_len = len;
    have_prev = 1;
    repeats = 1;
    return 0;
}

// Splits the input into lines as it arrives; -1 with errno set on a read error or no memory.
static int uniq_fd(int fd)
{
    char buf[4096];
    char *line = NULL;
    size_t len = 0, cap = 0;
    int n, rc = 0;

    while (rc == 0 && (n = read(fd, buf, sizeof(buf))) > 0) {
        for (int k = 0; k < n && rc == 0; k++) {
            if (buf[k] == '\n') {
                rc = take_line(line ? line : "", len);
                len = 0;
                continue;
            }
            if (len == cap) {
                char *bigger = realloc(line, cap ? cap * 2 : 256);
                if (!bigger) {
                    rc = -1;
                    break;
                }
                line = bigger;
                cap = cap ? cap * 2 : 256;
            }
            line[len++] = buf[k];
        }
    }
    if (rc == 0 && n < 0) {
        rc = -1;
    } else if (rc != 0) {
        errno = ENOMEM;
    } else if (len > 0) {
        rc = take_line(line, len);
        if (rc != 0) {
            errno = ENOMEM;
        }
    }
    free(line);
    return rc;
}

int main(int argc, char **argv)
{
    int i = 1;
    if (i < argc && strcmp(argv[i], "-c") == 0) {
        show_count = 1;
        i++;
    }

    int fd = STDIN_FILENO;
    const char *name = "-";
    if (i < argc) {
        name = argv[i];
        fd = open(name, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "uniq: %s: %s\n", name, strerror(errno));
            return 1;
        }
    }

    if (uniq_fd(fd) < 0) {
        fprintf(stderr, "uniq: %s: %s\n", name, strerror(errno));
        return 1;
    }
    flush_prev();
    return 0;
}
