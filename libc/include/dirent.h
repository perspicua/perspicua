/*
 * dirent.h - POSIX-like directory reading API
 */

#ifndef PERSPICUA_LIBC_DIRENT_H
#define PERSPICUA_LIBC_DIRENT_H

#include "syscall.h"

typedef struct {
    int fd;
    struct dirent *buffer;
    int num_dirents;
    int buffer_pos;
    int buffer_end;
} DIR;

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);

#endif // PERSPICUA_LIBC_DIRENT_H
