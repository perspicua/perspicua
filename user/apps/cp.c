#include "syscall.h"
#include "string.h"
#include "stdio.h"
#include "uapi/stat.h"

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int is_dir(const char *path)
{
    struct stat st;
    return sys_stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int copy_file(const char *src, const char *dst)
{
    int in = sys_open(src, VFS_O_RDONLY);
    if (in < 0) {
        printf("cp: cannot open '%s'\n", src);
        return 1;
    }
    int out = sys_open(dst, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (out < 0) {
        printf("cp: cannot create '%s'\n", dst);
        sys_close(in);
        return 1;
    }

    char buf[4096];
    int n, rc = 0;
    while ((n = sys_read(in, buf, sizeof(buf))) > 0) {
        int off = 0;
        while (off < n) {
            int w = sys_write(out, buf + off, n - off);
            if (w <= 0) {
                printf("cp: write error on '%s'\n", dst);
                rc = 1;
                goto done;
            }
            off += w;
        }
    }
    if (n < 0) {
        printf("cp: read error on '%s'\n", src);
        rc = 1;
    }
done:
    sys_close(in);
    sys_close(out);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: cp SRC DST\n");
        return 1;
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    /* cp SRC DIR copies into DIR under SRC's basename. */
    char target[512];
    if (is_dir(dst)) {
        int n = strlen(dst);
        strncpy(target, dst, sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';
        if (n > 0 && dst[n - 1] != '/') {
            strncat(target, "/", sizeof(target) - strlen(target) - 1);
        }
        strncat(target, basename_of(src), sizeof(target) - strlen(target) - 1);
        dst = target;
    }

    return copy_file(src, dst);
}
