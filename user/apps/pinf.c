#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "types.h"

static int read_proc_file(const char *path, char *buf, size_t bufsz)
{
    int fd = sys_open(path, VFS_O_RDONLY);
    if (fd < 0)
        return -1;
    int n = sys_read(fd, buf, bufsz - 1);
    if (n > 0)
        buf[n] = '\0';
    else
        buf[0] = '\0';
    sys_close(fd);
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: pinf <pid>\n");
        return 1;
    }

    char *pid_str = argv[1];
    char path[128];
    char buf[2048];

    printf("\n PROCESS INSPECTOR: PID %s\n", pid_str);
    printf(" ──────────────────────────\n");

    /* Status */
    snprintf(path, sizeof(path), "/proc/%s/status", pid_str);
    if (read_proc_file(path, buf, sizeof(buf)) > 0) {
        printf("%s", buf);
    } else {
        printf(" Error: Could not read status for PID %s\n", pid_str);
        return 1;
    }

    /* CMDLine */
    snprintf(path, sizeof(path), "/proc/%s/cmdline", pid_str);
    if (read_proc_file(path, buf, sizeof(buf)) > 0) {
        printf("Cmdline: %s\n", buf[0] ? buf : "[none]");
    }

    /* CWD */
    snprintf(path, sizeof(path), "/proc/%s/cwd", pid_str);
    if (read_proc_file(path, buf, sizeof(buf)) > 0) {
        printf("Cwd:     %s", buf); // already has newline from procfs
    }

    /* Maps */
    printf("\n VIRTUAL MEMORY MAP\n");
    printf(" ──────────────────\n");
    snprintf(path, sizeof(path), "/proc/%s/maps", pid_str);
    if (read_proc_file(path, buf, sizeof(buf)) > 0) {
        printf("%s", buf);
    } else {
        printf(" [none or inaccessible]\n");
    }

    /* Open FDs */
    printf("\n OPEN FILE DESCRIPTORS\n");
    printf(" ──────────────────────\n");
    snprintf(path, sizeof(path), "/proc/%s/fd", pid_str);
    int fd = sys_open(path, VFS_O_RDONLY);
    if (fd >= 0) {
        struct vfs_dirent dent;
        int found = 0;
        while (sys_getdents(fd, &dent, sizeof(dent)) > 0) {
            if (dent.name[0] == '.')
                continue;

            char fd_path[256];
            snprintf(fd_path, sizeof(fd_path), "/proc/%s/fd/%s", pid_str, dent.name);
            char link_target[512];
            if (read_proc_file(fd_path, link_target, sizeof(link_target)) > 0) {
                printf(" fd %3s -> %s", dent.name, link_target);
                found = 1;
            }
        }
        if (!found)
            printf(" [none]\n");
        sys_close(fd);
    } else {
        printf(" [none or inaccessible]\n");
    }
    printf("\n");

    return 0;
}
