/*
 * cat.c - Copies files, or stdin, to stdout.
 */

#include <stddef.h>

#include "errno.h"
#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

// Returns 0 once fd has been copied through to its end.
static int cat_file(int fd, const char *name)
{
    char *buf = malloc(4096);
    if (!buf) {
        fprintf(stderr, "cat: memory allocation failed\n");
        return 1;
    }
    int bytes_read;
    while ((bytes_read = read(fd, buf, 4096)) > 0) {
        fwrite(buf, 1, (size_t)bytes_read, stdout);
    }
    free(buf);
    if (bytes_read < 0) {
        fprintf(stderr, "cat: %s: %s\n", name, strerror(errno));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        return cat_file(0, "-");
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            rc |= cat_file(0, "-");
            continue;
        }
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        rc |= cat_file(fd, argv[i]);
        close(fd);
    }
    return rc;
}
