#include "syscall.h"
#include "string.h"
#include "stdio.h"

/* Classic `hexdump -C` layout: 8-digit offset, 16 hex bytes split into two
 * groups of eight, and the printable-ASCII rendering between | bars. Runs of
 * identical 16-byte lines collapse to a single '*', as hexdump does. */
static void print_line(unsigned long offset, const unsigned char *buf, int n)
{
    printf("%08lx  ", offset);
    for (int i = 0; i < 16; i++) {
        if (i < n)
            printf("%02x ", buf[i]);
        else
            printf("   ");
        if (i == 7)
            printf(" "); /* gap between the two 8-byte halves */
    }
    printf(" |");
    for (int i = 0; i < n; i++)
        printf("%c", (buf[i] >= 32 && buf[i] <= 126) ? buf[i] : '.');
    printf("|\n");
}

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

    unsigned char buf[16], prev[16];
    int n, have_prev = 0, squelched = 0;
    unsigned long offset = 0;

    while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
        /* Only full 16-byte lines participate in collapsing, so a partial final
         * line is always shown. */
        if (n == 16 && have_prev && memcmp(buf, prev, 16) == 0) {
            if (!squelched) {
                printf("*\n");
                squelched = 1;
            }
            offset += (unsigned long)n;
            continue;
        }

        squelched = 0;
        print_line(offset, buf, n);
        memcpy(prev, buf, (size_t)n);
        have_prev = (n == 16);
        offset += (unsigned long)n;
    }

    printf("%08lx\n", offset);
    sys_close(fd);
    return 0;
}
