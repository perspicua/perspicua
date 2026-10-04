/*
 * syscall.h - Umbrella over the POSIX headers.
 *
 * Every declaration here lives in the header POSIX puts it in; this exists so
 * a program that wants the whole set does not have to name all of them. New
 * code should include what it uses.
 */

#ifndef PERSPICUA_LIBC_SYSCALL_H
#define PERSPICUA_LIBC_SYSCALL_H

/*
 * Guard the userspace/kernel boundary at compile time. Without this, a
 * syscall-dependent source added to LIBC_SRC_COMMON still compiles into libk
 * and only fails at the unrelated, much later link that first references it.
 * Kernel code dispatches syscalls through core/syscall.h instead.
 */
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
