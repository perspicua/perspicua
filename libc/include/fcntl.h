/*
 * fcntl.h - Opening files and manipulating descriptors.
 */

#ifndef PERSPICUA_LIBC_FCNTL_H
#define PERSPICUA_LIBC_FCNTL_H

#include "uapi/fcntl.h"

int open(const char *path, int flags);
int fcntl(int fd, int cmd, int arg);

#endif // PERSPICUA_LIBC_FCNTL_H
