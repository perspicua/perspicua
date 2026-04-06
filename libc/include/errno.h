/*
 * errno.h - POSIX error number definitions and errno declaration.
 *
 * Provides the standard error constants and the per-process errno variable.
 * Error codes use the same numeric values as Linux/AArch64 for portability.
 *
 * Kernel code (compiled with __KERNEL__) must not use errno; it uses the
 * PERS_ERR_* codes from uapi/errors.h directly.
 */

#ifndef PERSPICUA_LIBC_ERRNO_H
#define PERSPICUA_LIBC_ERRNO_H

#ifndef __KERNEL__
extern int errno;
#endif

/* POSIX-compatible error numbers (AArch64) */
#define EPERM        1  /* Operation not permitted */
#define ENOENT       2  /* No such file or directory */
#define ESRCH        3  /* No such process */
#define EINTR        4  /* Interrupted system call */
#define EIO          5  /* I/O error */
#define ENXIO        6  /* No such device or address */
#define E2BIG        7  /* Argument list too long */
#define ENOEXEC      8  /* Exec format error */
#define EBADF        9  /* Bad file descriptor */
#define ECHILD       10 /* No child processes */
#define EAGAIN       11 /* Resource temporarily unavailable */
#define ENOMEM       12 /* Out of memory */
#define EACCES       13 /* Permission denied */
#define EFAULT       14 /* Bad address */
#define EBUSY        16 /* Device or resource busy */
#define EEXIST       17 /* File exists */
#define EXDEV        18 /* Cross-device link */
#define ENODEV       19 /* No such device */
#define ENOTDIR      20 /* Not a directory */
#define EISDIR       21 /* Is a directory */
#define EINVAL       22 /* Invalid argument */
#define ENFILE       23 /* Too many open files in system */
#define EMFILE       24 /* Too many open files */
#define ENOTTY       25 /* Inappropriate ioctl for device */
#define EFBIG        27 /* File too large */
#define ENOSPC       28 /* No space left on device */
#define ESPIPE       29 /* Illegal seek */
#define EROFS        30 /* Read-only file system */
#define EPIPE        32 /* Broken pipe */
#define ERANGE       34 /* Result too large */
#define EDEADLK      35 /* Resource deadlock avoided */
#define ENAMETOOLONG 36 /* File name too long */
#define ENOSYS       38 /* Function not implemented */
#define ENOTEMPTY    39 /* Directory not empty */
#define ELOOP        40 /* Too many levels of symbolic links */
#define EWOULDBLOCK  EAGAIN
#define ENOTSUP      95 /* Operation not supported */
#define EOPNOTSUPP   ENOTSUP
#define ETIMEDOUT    110 /* Connection timed out */
#define ECONNREFUSED 111 /* Connection refused */

#endif /* PERSPICUA_LIBC_ERRNO_H */
