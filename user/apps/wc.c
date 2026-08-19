#include "syscall.h"
#include "stdio.h"

static int want_l, want_w, want_c;

static void count_fd(int fd, unsigned long *lines, unsigned long *words, unsigned long *bytes)
{
    char buf[4096];
    int n, in_word = 0;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
        *bytes += (unsigned long)n;
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                (*lines)++;
            }
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                (*words)++;
            }
        }
    }
}

static void print_counts(unsigned long l, unsigned long w, unsigned long b, const char *name)
{
    if (want_l)
        printf("%8lu", l);
    if (want_w)
        printf("%8lu", w);
    if (want_c)
        printf("%8lu", b);
    if (name)
        printf(" %s", name);
    printf("\n");
}

/* A column header makes the numbers legible, but only at a terminal — piping to
 * another program must stay clean, so we suppress it unless stdout is a tty. */
static void print_header(int have_names)
{
    if (want_l)
        printf("%8s", "LINES");
    if (want_w)
        printf("%8s", "WORDS");
    if (want_c)
        printf("%8s", "BYTES");
    if (have_names)
        printf(" FILE");
    printf("\n");
}

/* tcgetpgrp succeeds only on a real terminal (the kernel checks the fd's node is
 * the tty device), so it distinguishes a tty from a pipe or file — which plain
 * S_ISCHR does not, since pipes report as character devices too. */
static int stdout_is_tty(void)
{
    return sys_tcgetpgrp(1) >= 0;
}

int main(int argc, char **argv)
{
    int start = 1;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'l')
                    want_l = 1;
                else if (argv[i][j] == 'w')
                    want_w = 1;
                else if (argv[i][j] == 'c')
                    want_c = 1;
            }
            start++;
        } else {
            break;
        }
    }
    if (!want_l && !want_w && !want_c) {
        want_l = want_w = want_c = 1;
    }

    int have_files = (start < argc);
    if (stdout_is_tty()) {
        print_header(have_files);
    }

    if (!have_files) {
        unsigned long l = 0, w = 0, b = 0;
        count_fd(0, &l, &w, &b);
        print_counts(l, w, b, NULL);
        return 0;
    }

    unsigned long tl = 0, tw = 0, tb = 0;
    int rc = 0;
    for (int i = start; i < argc; i++) {
        int fd = sys_open(argv[i], VFS_O_RDONLY);
        if (fd < 0) {
            printf("wc: %s: no such file\n", argv[i]);
            rc = 1;
            continue;
        }
        unsigned long l = 0, w = 0, b = 0;
        count_fd(fd, &l, &w, &b);
        sys_close(fd);
        print_counts(l, w, b, argv[i]);
        tl += l;
        tw += w;
        tb += b;
    }

    if (argc - start > 1) {
        print_counts(tl, tw, tb, "total");
    }
    return rc;
}
