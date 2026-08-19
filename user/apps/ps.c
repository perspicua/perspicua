#include "syscall.h"
#include "string.h"
#include "stdio.h"
#include "dirent.h"

/* Copy the value following "Key:" out of a /proc status blob. */
static void field(const char *blob, const char *key, char *out, int max)
{
    out[0] = '\0';
    const char *p = strstr(blob, key);
    if (!p)
        return;
    p += strlen(key);
    while (*p == ' ' || *p == '\t')
        p++;
    int i = 0;
    while (*p && *p != '\n' && i < max - 1)
        out[i++] = *p++;
    out[i] = '\0';
}

int main(void)
{
    DIR *d = opendir("/proc");
    if (!d) {
        printf("ps: cannot open /proc\n");
        return 1;
    }

    printf("%5s %5s %-9s %9s  %s\n", "PID", "PPID", "STATE", "MEM", "CMD");

    struct vfs_dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->name[0] < '0' || e->name[0] > '9') {
            continue; /* only numeric process directories */
        }

        char path[256];
        snprintf(path, sizeof(path), "/proc/%s/status", e->name);
        int fd = sys_open(path, VFS_O_RDONLY);
        if (fd < 0) {
            continue; /* process may have exited between readdir and open */
        }

        char blob[512];
        int n = sys_read(fd, blob, sizeof(blob) - 1);
        sys_close(fd);
        if (n <= 0) {
            continue;
        }
        blob[n] = '\0';

        char pid[16], ppid[16], state[16], name[64], mem[24];
        field(blob, "Pid:", pid, sizeof(pid));
        field(blob, "PPid:", ppid, sizeof(ppid));
        field(blob, "State:", state, sizeof(state));
        field(blob, "Name:", name, sizeof(name));
        field(blob, "VmSize:", mem, sizeof(mem));
        if (mem[0] == '\0') {
            strcpy(mem, "-");
        }
        printf("%5s %5s %-9s %9s  %s\n", pid, ppid, state, mem, name);
    }

    closedir(d);
    return 0;
}
