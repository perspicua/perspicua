/*
 * cp.c - Copies a file, into a directory under its own name when the destination is one.
 */

#include <stddef.h>

#include "syscall.h"
#include "string.h"
#include "stdio.h"
#include "uapi/stat.h"

#define PATH_LEN 512

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

// Absolute, with ".", ".." and repeated slashes folded away; -1 if it does not fit.
static int canonical(const char *path, char *out, size_t size)
{
    char joined[2 * PATH_LEN];
    char cwd[PATH_LEN];
    int n;
    if (path[0] == '/') {
        n = snprintf(joined, sizeof(joined), "%s", path);
    } else if (getcwd(cwd, sizeof(cwd))) {
        n = snprintf(joined, sizeof(joined), "%s/%s", cwd, path);
    } else {
        return -1;
    }
    if (n < 0 || n >= (int)sizeof(joined)) {
        return -1;
    }

    size_t len = 0;
    for (const char *p = joined; *p;) {
        while (*p == '/') {
            p++;
        }
        const char *name = p;
        while (*p && *p != '/') {
            p++;
        }
        size_t name_len = (size_t)(p - name);
        if (name_len == 0 || (name_len == 1 && name[0] == '.')) {
            continue;
        }
        if (name_len == 2 && name[0] == '.' && name[1] == '.') {
            while (len > 0 && out[len - 1] != '/') {
                len--;
            }
            if (len > 0) {
                len--;
            }
            continue;
        }
        if (len + 1 + name_len + 1 > size) {
            return -1;
        }
        out[len++] = '/';
        memcpy(out + len, name, name_len);
        len += name_len;
    }
    if (len == 0) {
        out[len++] = '/';
    }
    out[len] = '\0';
    return 0;
}

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

// FAT32 matches names regardless of case, so this compares them the same way.
static int same_path(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (lower(*a) != lower(*b)) {
            return 0;
        }
    }
    return *a == *b;
}

static int copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "cp: cannot open '%s'\n", src);
        return 1;
    }

    // Opening dst truncates it, so a dst that is src must be caught before that.
    struct stat st;
    if (stat(dst, &st) == 0) {
        char a[PATH_LEN], b[PATH_LEN];
        if (canonical(src, a, sizeof(a)) < 0 || canonical(dst, b, sizeof(b)) < 0) {
            fprintf(stderr, "cp: path too long\n");
            close(in);
            return 1;
        }
        if (same_path(a, b)) {
            fprintf(stderr, "cp: '%s' and '%s' are the same file\n", src, dst);
            close(in);
            return 1;
        }
    }

    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        fprintf(stderr, "cp: cannot create '%s'\n", dst);
        close(in);
        return 1;
    }

    char buf[4096];
    int n, rc = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        int off = 0;
        while (off < n) {
            int w = write(out, buf + off, n - off);
            if (w <= 0) {
                fprintf(stderr, "cp: write error on '%s'\n", dst);
                rc = 1;
                goto done;
            }
            off += w;
        }
    }
    if (n < 0) {
        fprintf(stderr, "cp: read error on '%s'\n", src);
        rc = 1;
    }
done:
    close(in);
    close(out);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: cp SRC DST\n");
        return 1;
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    char target[PATH_LEN];
    if (is_dir(dst)) {
        size_t n = strlen(dst);
        const char *sep = (n > 0 && dst[n - 1] == '/') ? "" : "/";
        int len = snprintf(target, sizeof(target), "%s%s%s", dst, sep, basename_of(src));
        if (len < 0 || len >= (int)sizeof(target)) {
            fprintf(stderr, "cp: path too long: '%s'\n", dst);
            return 1;
        }
        dst = target;
    }

    return copy_file(src, dst);
}
