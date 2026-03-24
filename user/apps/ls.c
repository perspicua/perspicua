#include "syscall.h"
#include "string.h"
#include "stdio.h"

static void print_string(const char* s)
{
    sys_write(1, s, strlen(s));
}

int main(int argc, char** argv)
{
    const char* path = ".";
    if (argc > 1)
    {
        path = argv[1];
    }

    int fd = sys_open(path, VFS_O_RDONLY);
    if (fd < 0)
    {
        print_string("ls: cannot open directory: ");
        print_string(path);
        print_string("\n");
        return 1;
    }

    struct vfs_dirent dirents[16];
    int res;
    while ((res = sys_getdents(fd, dirents, sizeof(dirents))) > 0)
    {
        for (int i = 0; i < res; i++)
        {
            print_string(dirents[i].name);
            print_string("\n");
        }
    }

    sys_close(fd);
    return 0;
}
