/*
 * errno.h - POSIX error reporting for userspace.
 *
 * The numbers live in uapi/errno.h because the kernel returns them; this adds
 * the variable a program reads after a failed call.
 */

#ifndef PERSPICUA_LIBC_ERRNO_H
#define PERSPICUA_LIBC_ERRNO_H

#include "uapi/errno.h"

#ifndef __KERNEL__
extern int errno;
#endif

#endif // PERSPICUA_LIBC_ERRNO_H
