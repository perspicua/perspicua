/*
 * syscall.h - Umbrella over the POSIX headers; new code includes what it uses instead.
 */

#ifndef PERSPICUA_LIBC_SYSCALL_H
#define PERSPICUA_LIBC_SYSCALL_H

// A syscall-dependent source added to libk fails here, not at a much later link.
#ifdef __KERNEL__
    #error "syscall.h is userspace-only; kernel code uses core/syscall.h"
#endif

#include <stddef.h>
#include <stdint.h>

#include "uapi/types.h"
#include "uapi/dirent.h"

#include "fcntl.h"
#include "sched.h"
#include "signal.h"
#include "time.h"
#include "stdio.h"
#include "unistd.h"
#include "sys/mman.h"
#include "sys/stat.h"
#include "sys/time.h"
#include "sys/wait.h"

#endif // PERSPICUA_LIBC_SYSCALL_H
