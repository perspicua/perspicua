#include "syscall.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "dirent.h"
#include "uapi/stat.h"

#define MAX_ENTRIES 256
#define PREFIX_MAX  512
#define MAX_DEPTH   32

static int show_all = 0;
static unsigned long dir_count = 0;
static unsigned long file_count = 0;

static int is_dir(const char *path)
{
    struct stat st;
    return sys_stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/*
 * Recursively print the contents of `path`, each line preceded by `prefix`.
 * The connector (├──/└──) marks whether an entry is the last in its directory,
 * and the child prefix extends with "│   " or "    " to keep the vertical bars
 * lined up beneath continuing branches.
 */
static void walk(const char *path, const char *prefix, int depth)
{
    if (depth > MAX_DEPTH)
        return;

    DIR *d = opendir(path);
    if (!d)
        return;

    char **names = malloc(sizeof(char *) * MAX_ENTRIES);
    if (!names) {
        closedir(d);
        return;
    }

    int count = 0;
    struct vfs_dirent *e;
    while ((e = readdir(d)) != NULL && count < MAX_ENTRIES) {
        if (strcmp(e->name, ".") == 0 || strcmp(e->name, "..") == 0)
            continue;
        if (e->name[0] == '.' && !show_all)
            continue;
        names[count++] = strdup(e->name);
    }
    closedir(d);

    for (int i = 0; i < count; i++) {
        int last = (i == count - 1);
        printf("%s%s%s\n", prefix, last ? "└── " : "├── ", names[i]);

        char child[512];
        snprintf(child, sizeof(child), "%s/%s", path, names[i]);
        if (is_dir(child)) {
            dir_count++;
            char child_prefix[PREFIX_MAX];
            snprintf(child_prefix, sizeof(child_prefix), "%s%s", prefix, last ? "    " : "│   ");
            walk(child, child_prefix, depth + 1);
        } else {
            file_count++;
        }
    }

    for (int i = 0; i < count; i++)
        free(names[i]);
    free(names);
}

int main(int argc, char **argv)
{
    const char *root = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0)
            show_all = 1;
        else
            root = argv[i];
    }
    if (!root)
        root = ".";

    struct stat st;
    if (sys_stat(root, &st) < 0) {
        printf("tree: %s: No such file or directory\n", root);
        return 1;
    }

    printf("%s\n", root);
    if (S_ISDIR(st.st_mode))
        walk(root, "", 0);

    printf("\n%lu %s, %lu %s\n", dir_count, dir_count == 1 ? "directory" : "directories",
           file_count, file_count == 1 ? "file" : "files");
    return 0;
}
