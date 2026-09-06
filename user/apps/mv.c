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

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: mv SRC DST\n");
        return 1;
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    /* mv SRC DIR moves into DIR under SRC's basename. */
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

    if (sys_rename(src, dst) < 0) {
        printf("mv: cannot move '%s' to '%s'\n", src, dst);
        return 1;
    }
    return 0;
}
