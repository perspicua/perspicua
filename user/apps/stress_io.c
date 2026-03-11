#include "syscall.h"
#include "string.h"

void print(const char* s)
{
    sys_write(1, s, strlen(s));
}

// WARNING: AFTER RUNNING THIS TEST, THERE IS A POSSIBLE MEMORY LEAK IN THE KERNEL
// THE HEAP USSAGE STAYS AT ABOUT 300KB
// THERE ARE THREE POSSIBLE CASES:
// 1. THIS TEST IS SHIT (not likely, the code is small and seems to be good)
// 2. THE DISPLAY IN THE KERNEL DASHBOARD IS BROKEN (more probable, worth checking out)
// 3. THERE IS AN ACTUAL LEAK IN THE I/O (check this one last, as it might be the hardest to track down)
int main(void)
{
    print("STRESS: Starting I/O Stress Test (VFS/Devs)...\n");

    const char* ramfs_file = "/hello.txt";
    const char* uart_file = "/dev/uart";
    char buf[256];
    const char* data = "STRESSING_KERNEL_IO_PATH_WITH_REPEATED_WRITES\n";
    size_t data_len = strlen(data);

    for (int i = 0; i < 5000; i++)
    {
        int fd_read = sys_open(ramfs_file, O_RDONLY);
        if (fd_read >= 0)
        {
            sys_read(fd_read, buf, sizeof(buf));
            sys_close(fd_read);
        }

        int fd_write = sys_open(uart_file, O_WRONLY);
        if (fd_write >= 0)
        {
            for (int j = 0; j < 5; j++)
            {
                sys_write(fd_write, data, data_len);
            }
            sys_close(fd_write);
        }

        sys_open("/non-existent-file-path", O_RDONLY);

        if (i % 100 == 0)
        {
            print("  completed 100 I/O cycles\n");
            sys_yield();
        }
    }

    print("STRESS: I/O test complete.\n");
    sys_exit();
    return 0;
}
