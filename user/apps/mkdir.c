/*
 * mkdir.c - Creates directories, and with -p every missing parent too.
 */

#include "errno.h"
#include "syscall.h"
#include "string.h"
#include "stdio.h"
#include "uapi/stat.h"

// Create every missing parent of path; the final mkdir reports any that could not be made.
static void make_parents(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(path, 0755);
            *p = '/';
        }
    }
}

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int main(int argc, char **argv)
{
    int parents = 0;
    int start = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0) {
            parents = 1;
            start++;
        } else {
            break;
        }
    }

    if (start >= argc) {
        fprintf(stderr, "usage: mkdir [-p] DIR...\n");
        return 1;
    }

    int rc = 0;
    for (int i = start; i < argc; i++) {
        if (parents) {
            make_parents(argv[i]);
        }
        if (mkdir(argv[i], 0755) == 0) {
            continue;
        }
        // -p is satisfied by a directory that already exists, and only by a directory.
        int err = errno;
        if (parents && err == EEXIST && is_dir(argv[i])) {
            continue;
        }
        fprintf(stderr, "mkdir: cannot create directory '%s': %s\n", argv[i], strerror(err));
        rc = 1;
    }
    return rc;
}
