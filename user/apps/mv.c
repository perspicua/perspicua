/*
 * mv.c - Renames a file, into a directory under its own name when the destination is one.
 */

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
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: mv SRC DST\n");
        return 1;
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    // Into DIR under SRC's basename; a truncated target would be a name nobody asked for.
    char target[512];
    if (is_dir(dst)) {
        size_t n = strlen(dst);
        const char *sep = (n > 0 && dst[n - 1] == '/') ? "" : "/";
        int len = snprintf(target, sizeof(target), "%s%s%s", dst, sep, basename_of(src));
        if (len < 0 || len >= (int)sizeof(target)) {
            fprintf(stderr, "mv: path too long: '%s'\n", dst);
            return 1;
        }
        dst = target;
    }

    if (rename(src, dst) < 0) {
        fprintf(stderr, "mv: cannot move '%s' to '%s'\n", src, dst);
        return 1;
    }
    return 0;
}
