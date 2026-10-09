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

#define LINE_MAX 1024

static int show_count;
static char prev[LINE_MAX];
static int have_prev;
static long repeats;

static void flush_prev(void)
{
    if (!have_prev) {
        return;
    }
    if (show_count) {
        printf("%7ld %s\n", repeats, prev);
    } else {
        printf("%s\n", prev);
    }
}

static void take_line(const char *line)
{
    if (have_prev && strcmp(line, prev) == 0) {
        repeats++;
        return;
    }
    flush_prev();
    strncpy(prev, line, sizeof(prev) - 1);
    prev[sizeof(prev) - 1] = '\0';
    have_prev = 1;
    repeats = 1;
}

// Splits the input into lines as it arrives; longer lines are cut at LINE_MAX - 1.
static int uniq_fd(int fd)
{
    char buf[4096];
    char line[LINE_MAX];
    size_t len = 0;
    int n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int k = 0; k < n; k++) {
            if (buf[k] == '\n') {
                line[len] = '\0';
                take_line(line);
                len = 0;
            } else if (len < sizeof(line) - 1) {
                line[len++] = buf[k];
            }
        }
    }
    if (len > 0) {
        line[len] = '\0';
        take_line(line);
    }
    return n < 0 ? -1 : 0;
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
