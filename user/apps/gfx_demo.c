#include <stddef.h>
#include <stdint.h>

#include "syscall.h"
#include "uapi/mman.h"

#define WIDTH   1024
#define HEIGHT  768
#define BPP     4
#define FB_SIZE (WIDTH * HEIGHT * BPP)

int main(void)
{
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        char err[] = "Error: could not open /dev/fb0\n";
        write(1, err, sizeof(err) - 1);
        _exit(1);
    }

    uint32_t *fb = (uint32_t *)mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        char err[] = "Error: could not mmap framebuffer\n";
        write(1, err, sizeof(err) - 1);
        close(fd);
        _exit(1);
    }

    uint32_t color = 0xFFFF0000;
    int r = 0xFF;
    int dr = -1;

    for (int frame = 0; frame < 200; frame++) {
        r += dr;
        if (r <= 0 || r >= 255) {
            dr = -dr;
        }
        color = 0xFF000000 | (r << 16);

        for (int i = 0; i < WIDTH * HEIGHT; i++) {
            fb[i] = color;
        }

        usleep((10) * 1000); // Wait 10ms
    }

    char done[] = "GFX Demo finished.\n";
    write(1, done, sizeof(done) - 1);

    close(fd);
    _exit(0);
    return 0;
}
