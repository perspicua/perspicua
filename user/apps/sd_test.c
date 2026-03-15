#include "syscall.h"
#include "stdio.h"
#include "string.h"

int main(void)
{
    printf("[SDTEST] Starting SD card user-space test...\n");

    int fd = sys_open("/dev/sd0", VFS_O_RDWR);
    if (fd < 0)
    {
        printf("[SDTEST] Error: Could not open /dev/sd0\n");
        sys_exit(1);
    }

    printf("[SDTEST] Opened /dev/sd0 (fd: %d)\n", fd);

    static uint8_t write_buf[512];
    for (int i = 0; i < 512; i++)
    {
        write_buf[i] = (uint8_t)(i & 0xFF);
    }

    const char marker[] = "USERSPACE SD TEST MARKER";
    memcpy(write_buf, marker, sizeof(marker));

    printf("[SDTEST] Writing 512 bytes to block 0...\n");
    sys_write(fd, (const char*)write_buf, 512);

    sys_close(fd);
    fd = sys_open("/dev/sd0", VFS_O_RDONLY);
    if (fd < 0)
    {
        printf("[SDTEST] Error: Could not re-open /dev/sd0\n");
        sys_exit(1);
    }

    printf("[SDTEST] Reading back 512 bytes from block 0...\n");
    static uint8_t read_buf[512];
    int read_bytes = sys_read(fd, read_buf, 512);
    if (read_bytes <= 0)
    {
        printf("[SDTEST] Error: sys_read failed (returned %d)\n", read_bytes);
        sys_close(fd);
        sys_exit(1);
    }

    printf("[SDTEST] Verification: Marker is '%s'\n", (char*)read_buf);

    int errors = 0;
    for (int i = 0; i < 512; i++)
    {
        if (read_buf[i] != write_buf[i])
        {
            if (errors < 5)
            {
                printf("[SDTEST] Mismatch at offset %d: expected 0x%x, got 0x%x\n", i, write_buf[i], read_buf[i]);
            }
            errors++;
        }
    }

    if (errors == 0)
    {
        printf("[SDTEST] SUCCESS: Data verification passed!\n");
    }
    else
    {
        printf("[SDTEST] FAILURE: %d data mismatches found.\n", errors);
    }

    sys_close(fd);
    printf("[SDTEST] Test complete.\n");
    sys_exit(0);
    return 0;
}
