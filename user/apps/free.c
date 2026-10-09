/*
 * free - show how much memory is in use, from /proc/meminfo.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// The kB value on the line starting with key, or -1.
static long field(const char *text, const char *key)
{
    const char *at = strstr(text, key);
    if (!at) {
        return -1;
    }
    at += strlen(key);
    while (*at == ' ') {
        at++;
    }
    return atol(at);
}

int main(void)
{
    char buf[512];
    int fd = open("/proc/meminfo", O_RDONLY);
    int n = fd < 0 ? -1 : read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        fprintf(stderr, "free: cannot read /proc/meminfo\n");
        return 1;
    }
    close(fd);
    buf[n] = '\0';

    long total = field(buf, "MemTotal:");
    long avail = field(buf, "MemFree:");
    long slab_used = field(buf, "SlabUsed:");
    long slab_total = field(buf, "SlabTotal:");
    if (total < 0 || avail < 0) {
        fprintf(stderr, "free: /proc/meminfo is missing a field\n");
        return 1;
    }

    printf("          %10s %10s %10s\n", "total", "used", "free");
    printf("Mem:      %7ld MB %7ld MB %7ld MB\n", total / 1024, (total - avail) / 1024,
           avail / 1024);
    if (slab_used >= 0 && slab_total >= 0) {
        printf("Slab:     %7ld kB %7ld kB %7ld kB\n", slab_total, slab_used,
               slab_total - slab_used);
    }
    return 0;
}
