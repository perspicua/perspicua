#include "syscall.h"
#include "string.h"
#include "stdio.h"
#include "dirent.h"
#include "uapi/stat.h"

static int g_recursive = 0;
static int g_force = 0;

static int remove_path(const char *path);

/* Empty a directory recursively, then remove the directory itself. */
static int remove_dir(const char *path)
{
    DIR *dirp = opendir(path);
    if (!dirp) {
        if (!g_force)
            printf("rm: cannot open directory '%s'\n", path);
        return 1;
    }

    int rc = 0;
    struct vfs_dirent *ent;
    while ((ent = readdir(dirp)) != NULL) {
        if (strcmp(ent->name, ".") == 0 || strcmp(ent->name, "..") == 0) {
            continue;
        }
        char child[512];
        int n = strlen(path);
        strncpy(child, path, sizeof(child) - 1);
        child[sizeof(child) - 1] = '\0';
        if (n > 0 && path[n - 1] != '/') {
            strncat(child, "/", sizeof(child) - strlen(child) - 1);
        }
        strncat(child, ent->name, sizeof(child) - strlen(child) - 1);
        rc |= remove_path(child);
    }
    closedir(dirp);

    if (sys_rmdir(path) < 0) {
        if (!g_force)
            printf("rm: cannot remove directory '%s'\n", path);
        rc = 1;
    }
    return rc;
}

static int remove_path(const char *path)
{
    struct stat st;
    if (sys_stat(path, &st) < 0) {
        if (!g_force)
            printf("rm: cannot remove '%s': no such file\n", path);
        return g_force ? 0 : 1;
    }

    if (S_ISDIR(st.st_mode)) {
        if (!g_recursive) {
            printf("rm: cannot remove '%s': is a directory\n", path);
            return 1;
        }
        return remove_dir(path);
    }

    if (sys_unlink(path) < 0) {
        if (!g_force)
            printf("rm: cannot remove '%s'\n", path);
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
        if (g_force)
            return 0;
        printf("usage: rm [-rf] FILE...\n");
        return 1;
    }

    int rc = 0;
    for (int i = start; i < argc; i++) {
        rc |= remove_path(argv[i]);
    }
    return rc;
}
