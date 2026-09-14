/*
 * sys/stat.h - File metadata.
 */

#ifndef PERSPICUA_LIBC_SYS_STAT_H
#define PERSPICUA_LIBC_SYS_STAT_H

#include "uapi/stat.h"

int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
int mkdir(const char *path, int mode);

#endif // PERSPICUA_LIBC_SYS_STAT_H
