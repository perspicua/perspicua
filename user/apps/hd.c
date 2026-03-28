#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "types.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: hd <file>\n");
        return 1;
    }

    int fd = sys_open(argv[1], VFS_O_RDONLY);
    if (fd < 0) {
        printf("hd: cannot open %s\n", argv[1]);
        return 1;
    }

    unsigned char buf[8];
    int n;
    unsigned long offset = 0;

    printf("\n HEXDUMP (AArch64 64-bit Word Aligned)\n");
    printf(" ──────────────────────────────────────────\n");

    while ((n = sys_read(fd, buf, 8)) > 0) {
        printf(" %08lx | ", offset);

        for (int i = 0; i < 8; i++) {
            if (i < n)
                printf("%02x ", buf[i]);
            else
                printf("   ");
        }

        printf(" | ");
        for (int i = 0; i < n; i++) {
            if (buf[i] >= 32 && buf[i] <= 126)
                printf("%c", buf[i]);
            else
                printf(".");
        }
        printf("\n");
        offset += n;
    }

    sys_close(fd);
    printf("\n");
    return 0;
}
