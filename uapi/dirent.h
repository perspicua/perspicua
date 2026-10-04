/*
 * dirent.h - Directory entry as getdents returns it.
 *
 * Shared by the kernel and userspace: the kernel fills these in and copies
 * them out, so the layout is part of the syscall ABI.
 */

#ifndef PERSPICUA_UAPI_DIRENT_H
#define PERSPICUA_UAPI_DIRENT_H

#include <stdint.h>

#define NAME_MAX 255

struct dirent {
    uint32_t d_ino;
    char d_name[NAME_MAX + 1];
};

#endif // PERSPICUA_UAPI_DIRENT_H
