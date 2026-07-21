/*
 * dirent.c - POSIX-like directory reading API implementation
 */

#include "dirent.h"
#include "stdlib.h"
#include "syscall.h"

DIR *opendir(const char *name)
{
    int fd = sys_open(name, VFS_O_RDONLY);
    if (fd < 0) {
        return NULL;
    }

    DIR *dirp = malloc(sizeof(DIR));
    if (!dirp) {
        sys_close(fd);
        return NULL;
    }

    dirp->fd = fd;
    dirp->num_dirents = 32;
    dirp->buffer = malloc(sizeof(struct vfs_dirent) * dirp->num_dirents);
    if (!dirp->buffer) {
        free(dirp);
        sys_close(fd);
        return NULL;
    }

    dirp->buffer_pos = 0;
    dirp->buffer_end = 0;

    return dirp;
}

struct vfs_dirent *readdir(DIR *dirp)
{
    if (!dirp) {
        return NULL;
    }

    if (dirp->buffer_pos >= dirp->buffer_end) {
        int res =
            sys_getdents(dirp->fd, dirp->buffer, sizeof(struct vfs_dirent) * dirp->num_dirents);
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

    int res = sys_close(dirp->fd);
    free(dirp->buffer);
    free(dirp);

    return res < 0 ? -1 : 0;
}
