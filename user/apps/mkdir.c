#include "syscall.h"
#include "string.h"
#include "stdio.h"

/* Create every missing parent of path, ignoring components that already exist. */
static void make_parents(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            sys_mkdir(path, 0755);
            *p = '/';
        }
    }
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
        printf("usage: mkdir [-p] DIR...\n");
        return 1;
    }

    int rc = 0;
    for (int i = start; i < argc; i++) {
        if (parents) {
            make_parents(argv[i]);
            /* -p tolerates an already-existing final directory. */
            sys_mkdir(argv[i], 0755);
        } else if (sys_mkdir(argv[i], 0755) < 0) {
            printf("mkdir: cannot create directory '%s'\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}
