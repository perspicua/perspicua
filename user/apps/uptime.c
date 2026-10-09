/*
 * uptime - how long since boot, and how many processes are alive.
 */

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int count_processes(void)
{
    DIR *d = opendir("/proc");
    if (!d) {
        return -1;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        n += e->d_name[0] >= '1' && e->d_name[0] <= '9';
    }
    closedir(d);
    return n;
}

int main(void)
{
    char buf[64];
    int fd = open("/proc/uptime", O_RDONLY);
    int n = fd < 0 ? -1 : read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        fprintf(stderr, "uptime: cannot read /proc/uptime\n");
        return 1;
    }
    close(fd);
    buf[n] = '\0';

    long secs = atol(buf);
    long days = secs / 86400;
    long h = secs / 3600 % 24;
    long m = secs / 60 % 60;
    long s = secs % 60;

    if (days > 0) {
        printf("up %ld day%s, %ld:%02ld:%02ld", days, days == 1 ? "" : "s", h, m, s);
    } else {
        printf("up %ld:%02ld:%02ld", h, m, s);
    }
    int procs = count_processes();
    if (procs >= 0) {
        printf(", %d process%s", procs, procs == 1 ? "" : "es");
    }
    printf("\n");
    return 0;
}
