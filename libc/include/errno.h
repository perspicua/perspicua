/*
 * errno.h - errno over uapi/errno.h's numbers, which the kernel returns negated.
 */

#ifndef PERSPICUA_LIBC_ERRNO_H
#define PERSPICUA_LIBC_ERRNO_H

#include "uapi/errno.h"

#ifndef __KERNEL__
extern int errno;
#endif

#endif // PERSPICUA_LIBC_ERRNO_H
