/*
 * dirent.h - The directory entry getdents copies out, so its layout is syscall ABI.
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
