#include "syscall.h"
#include "stdio.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: touch FILE...\n");
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = sys_open(argv[i], VFS_O_WRONLY | VFS_O_CREAT);
        if (fd < 0) {
            printf("touch: cannot create '%s'\n", argv[i]);
            rc = 1;
            continue;
        }
        sys_close(fd);
    }
    return rc;
}
