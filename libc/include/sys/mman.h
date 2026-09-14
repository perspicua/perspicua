/*
 * sys/mman.h - Memory mapping.
 */

#ifndef PERSPICUA_LIBC_SYS_MMAN_H
#define PERSPICUA_LIBC_SYS_MMAN_H

#include <stddef.h>

#include "uapi/types.h"
#include "uapi/mman.h"

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);

#endif // PERSPICUA_LIBC_SYS_MMAN_H
