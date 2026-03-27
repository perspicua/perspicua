#include "syscall.h"
#include "stdio.h"
#include "string.h"

static void cat_file(int fd)
{
    char buf[512];
    int bytes_read;
    while ((bytes_read = sys_read(fd, buf, sizeof(buf))) > 0)
    {
        sys_write(1, buf, (size_t)bytes_read);
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        /* No arguments: read from stdin */
        cat_file(0);
        sys_exit(0);
    }

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-") == 0)
        {
            cat_file(0);
        }
        else
        {
            int fd = sys_open(argv[i], VFS_O_RDONLY);
            if (fd < 0)
            {
                char err_prefix[] = "cat: ";
                char err_suffix[] = ": No such file or directory\n";
                sys_write(2, err_prefix, strlen(err_prefix));
                sys_write(2, argv[i], strlen(argv[i]));
                sys_write(2, err_suffix, strlen(err_suffix));
                continue;
            }
            cat_file(fd);
            sys_close(fd);
        }
    }

    sys_exit(0);
    return 0;
}
