/*
 * touch.c - Creates each file that does not exist yet.
 */

#include "syscall.h"
#include "stdio.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: touch FILE...\n");
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) {
            fprintf(stderr, "touch: cannot create '%s'\n", argv[i]);
            rc = 1;
            continue;
        }
        close(fd);
    }
    return rc;
}
