#include "syscall.h"

int main(void)
{
    int fd = sys_open("/hello.txt", O_RDONLY);
    if (fd < 0)
    {
        char err[] = "Error: could not open /hello.txt\n";
        sys_write(1, err, sizeof(err) - 1);
        sys_exit();
    }

    char buf[64];
    int n;
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0)
    {
        sys_write(1, buf, (size_t)n);
    }

    sys_close(fd);
    sys_exit();
    return 0;
}
