/*
 * rm.c - Removes files, and with -r whole directory trees.
 */

#include <stddef.h>

#include "syscall.h"
#include "string.h"
#include "stdio.h"
#include "dirent.h"
#include "uapi/stat.h"

static int g_recursive = 0;
static int g_force = 0;

static int remove_path(const char *path);

// Empty a directory recursively, then remove the directory itself.
static int remove_dir(const char *path)
{
    DIR *dirp = opendir(path);
    if (!dirp) {
        if (!g_force) {
            fprintf(stderr, "rm: cannot open directory '%s'\n", path);
        }
        return 1;
    }

    int rc = 0;
    struct dirent *ent;
    while ((ent = readdir(dirp)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        // A truncated child path could name a different file.
        char child[512];
        size_t n = strlen(path);
        const char *sep = (n > 0 && path[n - 1] == '/') ? "" : "/";
        int len = snprintf(child, sizeof(child), "%s%s%s", path, sep, ent->d_name);
        if (len < 0 || len >= (int)sizeof(child)) {
            fprintf(stderr, "rm: path too long: '%s%s%s'\n", path, sep, ent->d_name);
            rc = 1;
            continue;
        }
        rc |= remove_path(child);
    }
    closedir(dirp);

    if (rmdir(path) < 0) {
        if (!g_force) {
            fprintf(stderr, "rm: cannot remove directory '%s'\n", path);
        }
        rc = 1;
    }
    return rc;
}

// "/" and any path ending in "." or ".." (trailing slashes aside) name a tree rm must not take.
static int is_protected(const char *path)
{
    size_t end = strlen(path);
    while (end > 0 && path[end - 1] == '/') {
        end--;
    }
    if (end == 0) {
        return path[0] == '/';
    }
    size_t start = end;
    while (start > 0 && path[start - 1] != '/') {
        start--;
    }
    size_t len = end - start;
    return (len == 1 && path[start] == '.')
           || (len == 2 && path[start] == '.' && path[start + 1] == '.');
}

static int remove_path(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        if (!g_force) {
            fprintf(stderr, "rm: cannot remove '%s': no such file\n", path);
        }
        return g_force ? 0 : 1;
    }

    if (S_ISDIR(st.st_mode)) {
        if (!g_recursive) {
            fprintf(stderr, "rm: cannot remove '%s': is a directory\n", path);
            return 1;
        }
        return remove_dir(path);
    }

    if (unlink(path) < 0) {
        if (!g_force) {
            fprintf(stderr, "rm: cannot remove '%s'\n", path);
        }
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int start = 1;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'r' || argv[i][j] == 'R') {
                    g_recursive = 1;
                } else if (argv[i][j] == 'f') {
                    g_force = 1;
                }
            }
            start++;
        } else {
            break;
        }
    }

    if (start >= argc) {
        if (g_force) {
            return 0;
        }
        fprintf(stderr, "usage: rm [-rf] FILE...\n");
        return 1;
    }

    int rc = 0;
    for (int i = start; i < argc; i++) {
        if (is_protected(argv[i])) {
            fprintf(stderr, "rm: refusing to remove '%s'\n", argv[i]);
            rc = 1;
            continue;
        }
        rc |= remove_path(argv[i]);
    }
    return rc;
}
