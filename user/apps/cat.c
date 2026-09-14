#include <stddef.h>

#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static void cat_file(int fd)
{
    char *buf = malloc(4096);
    if (!buf) {
        printf("cat: memory allocation failed\n");
        return;
    }
    int bytes_read;
    while ((bytes_read = read(fd, buf, 4096)) > 0) {
        write(1, buf, (size_t)bytes_read);
    }
    free(buf);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        // No arguments: read from stdin
        cat_file(0);
        _exit(0);
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            cat_file(0);
        } else {
            int fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                char err_prefix[] = "cat: ";
                char err_suffix[] = ": No such file or directory\n";
                write(2, err_prefix, strlen(err_prefix));
                write(2, argv[i], strlen(argv[i]));
                write(2, err_suffix, strlen(err_suffix));
                continue;
            }
            cat_file(fd);
            close(fd);
        }
    }

    _exit(0);
    return 0;
}
