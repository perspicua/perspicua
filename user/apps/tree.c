/*
 * tree.c - Draws a directory tree with box-drawing branches, then counts its directories and files.
 */

#include <stddef.h>

#include "syscall.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "dirent.h"
#include "uapi/stat.h"

#define PREFIX_MAX 512
#define MAX_DEPTH  32

static int show_all = 0;
static unsigned long dir_count = 0;
static unsigned long file_count = 0;
static int failed;

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// └── marks a directory's last entry; children extend prefix with "│   " or "    ".
static void walk(const char *path, const char *prefix, int depth)
{
    if (depth > MAX_DEPTH) {
        fprintf(stderr, "tree: %s: deeper than %d levels, not shown\n", path, MAX_DEPTH);
        failed = 1;
        return;
    }

    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "tree: %s: cannot open\n", path);
        failed = 1;
        return;
    }

    char **names = NULL;
    int count = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        if (e->d_name[0] == '.' && !show_all) {
            continue;
        }
        if (count == cap) {
            int bigger_cap = cap ? cap * 2 : 64;
            char **bigger = realloc(names, sizeof(char *) * (size_t)bigger_cap);
            if (!bigger) {
                break;
            }
            names = bigger;
            cap = bigger_cap;
        }
        if (!(names[count] = strdup(e->d_name))) {
            break;
        }
        count++;
    }
    if (e) {
        fprintf(stderr, "tree: %s: out of memory, listing cut short\n", path);
        failed = 1;
    }
    closedir(d);

    for (int i = 0; i < count; i++) {
        int last = (i == count - 1);
        printf("%s%s%s\n", prefix, last ? "└── " : "├── ", names[i]);

        char child[512];
        char child_prefix[PREFIX_MAX];
        if (snprintf(child, sizeof(child), "%s/%s", path, names[i]) >= (int)sizeof(child)
            || snprintf(child_prefix, sizeof(child_prefix), "%s%s", prefix, last ? "    " : "│   ")
                   >= (int)sizeof(child_prefix)) {
            fprintf(stderr, "tree: %s/%s: path too long\n", path, names[i]);
            failed = 1;
        } else if (is_dir(child)) {
            dir_count++;
            walk(child, child_prefix, depth + 1);
        } else {
            file_count++;
        }
    }

    for (int i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
}

int main(int argc, char **argv)
{
    const char *root = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            show_all = 1;
        } else {
            root = argv[i];
        }
    }
    if (!root) {
        root = ".";
    }

    struct stat st;
    if (stat(root, &st) < 0) {
        fprintf(stderr, "tree: %s: No such file or directory\n", root);
        return 1;
    }

    printf("%s\n", root);
    if (S_ISDIR(st.st_mode)) {
        walk(root, "", 0);
    }

    printf("\n%lu %s, %lu %s\n", dir_count, dir_count == 1 ? "directory" : "directories",
           file_count, file_count == 1 ? "file" : "files");
    return failed;
}
