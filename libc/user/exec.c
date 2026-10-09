/*
 * exec.c - Running a program by name through PATH.
 */

#include "errno.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"

#define PATH_BUF 256

int execvp(const char *file, char *const argv[])
{
    if (strchr(file, '/')) {
        return execve(file, argv, environ);
    }

    const char *path = getenv("PATH");
    if (!path) {
        path = "/bin:/";
    }

    static const char *const suffixes[] = {".elf", ""};
    size_t flen = strlen(file);
    char buf[PATH_BUF];

    while (*path) {
        const char *colon = strchr(path, ':');
        size_t dlen = colon ? (size_t)(colon - path) : strlen(path);
        int slash = dlen > 0 && path[dlen - 1] != '/';

        for (size_t k = 0; k < sizeof(suffixes) / sizeof(suffixes[0]); k++) {
            size_t slen = strlen(suffixes[k]);
            if (dlen + (size_t)slash + flen + slen >= sizeof(buf)) {
                continue;
            }
            memcpy(buf, path, dlen);
            size_t at = dlen;
            if (slash) {
                buf[at++] = '/';
            }
            memcpy(buf + at, file, flen);
            memcpy(buf + at + flen, suffixes[k], slen + 1);
            execve(buf, argv, environ);
        }

        path = colon ? colon + 1 : path + dlen;
    }

    errno = ENOENT;
    return -1;
}
