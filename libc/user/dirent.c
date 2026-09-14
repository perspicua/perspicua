/*
 * dirent.c - POSIX-like directory reading API implementation
 */

#include "dirent.h"
#include "stdlib.h"
#include "syscall.h"

#include <stddef.h>

DIR *opendir(const char *name)
{
    int fd = open(name, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }

    DIR *dirp = malloc(sizeof(DIR));
    if (!dirp) {
        close(fd);
        return NULL;
    }

    dirp->fd = fd;
    dirp->num_dirents = 32;
    dirp->buffer = malloc(sizeof(struct dirent) * dirp->num_dirents);
    if (!dirp->buffer) {
        free(dirp);
        close(fd);
        return NULL;
    }

    dirp->buffer_pos = 0;
    dirp->buffer_end = 0;

    return dirp;
}

struct dirent *readdir(DIR *dirp)
{
    if (!dirp) {
        return NULL;
    }

    if (dirp->buffer_pos >= dirp->buffer_end) {
        int res = getdents(dirp->fd, dirp->buffer, sizeof(struct dirent) * dirp->num_dirents);
        if (res <= 0) {
            return NULL;
        }
        dirp->buffer_pos = 0;
        dirp->buffer_end = res;
    }

    return &dirp->buffer[dirp->buffer_pos++];
}

int closedir(DIR *dirp)
{
    if (!dirp) {
        return -1;
    }

    int res = close(dirp->fd);
    free(dirp->buffer);
    free(dirp);

    return res < 0 ? -1 : 0;
}
