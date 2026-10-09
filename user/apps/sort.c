/*
 * sort - sort lines of text.
 *
 * -r reverses the order, -n compares the leading number of each line.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int reverse, numeric;

static char *text;
static size_t text_len, text_cap;

static int reserve(size_t extra)
{
    if (text_len + extra <= text_cap) {
        return 0;
    }
    size_t cap = text_cap ? text_cap : 4096;
    while (cap < text_len + extra) {
        cap *= 2;
    }
    char *bigger = realloc(text, cap);
    if (!bigger) {
        return -1;
    }
    text = bigger;
    text_cap = cap;
    return 0;
}

// Appends a whole input, ending it with a newline so the next one starts a new line.
static int append_fd(int fd)
{
    for (;;) {
        if (reserve(4096) < 0) {
            return -1;
        }
        int n = read(fd, text + text_len, text_cap - text_len);
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            break;
        }
        text_len += (size_t)n;
    }
    if (text_len > 0 && text[text_len - 1] != '\n') {
        text[text_len++] = '\n';
    }
    return 0;
}

// The leading integer, after blanks; a line without one counts as 0.
static long leading_number(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return atol(s);
}

static int compare(const void *a, const void *b)
{
    const char *x = *(const char *const *)a;
    const char *y = *(const char *const *)b;
    int c = 0;
    if (numeric) {
        long nx = leading_number(x);
        long ny = leading_number(y);
        c = nx < ny ? -1 : nx > ny;
    }
    if (c == 0) {
        c = strcmp(x, y);
    }
    return reverse ? -c : c;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'r') {
                reverse = 1;
            } else if (*p == 'n') {
                numeric = 1;
            } else {
                fprintf(stderr, "sort: invalid option -- '%c'\n", *p);
                return 2;
            }
        }
    }

    if (i == argc && append_fd(STDIN_FILENO) < 0) {
        fprintf(stderr, "sort: -: %s\n", strerror(errno));
        return 1;
    }
    for (; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0 || append_fd(fd) < 0) {
            fprintf(stderr, "sort: %s: %s\n", argv[i], strerror(errno));
            return 1;
        }
        close(fd);
    }

    size_t count = 0;
    for (size_t k = 0; k < text_len; k++) {
        count += text[k] == '\n';
    }
    char **lines = malloc((count ? count : 1) * sizeof(char *));
    if (!lines) {
        fprintf(stderr, "sort: out of memory\n");
        return 1;
    }

    size_t n = 0;
    char *start = text;
    for (size_t k = 0; k < text_len; k++) {
        if (text[k] == '\n') {
            text[k] = '\0';
            lines[n++] = start;
            start = text + k + 1;
        }
    }

    qsort(lines, n, sizeof(char *), compare);
    for (size_t k = 0; k < n; k++) {
        printf("%s\n", lines[k]);
    }
    return 0;
}
