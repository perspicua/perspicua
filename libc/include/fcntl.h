/*
 * fcntl.h - Opening files and manipulating descriptors.
 */

#ifndef PERSPICUA_LIBC_FCNTL_H
#define PERSPICUA_LIBC_FCNTL_H

#include "uapi/fcntl.h"

// A mode argument after O_CREAT is accepted for POSIX code and ignored: files have no modes.
int open(const char *path, int flags, ...);
int fcntl(int fd, int cmd, ...);

#endif // PERSPICUA_LIBC_FCNTL_H
