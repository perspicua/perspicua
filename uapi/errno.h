/*
 * errno.h - Error numbers, shared by the kernel and userspace.
 *
 * The values are Linux/AArch64's, so a program built elsewhere against those
 * headers sees the numbers it expects. The kernel returns them negated
 * (-ENOENT); libc negates them back into errno, which is why there is no
 * translation table between the two sides.
 *
 * Userspace includes <errno.h>, which adds errno itself on top of this.
 */

#ifndef PERSPICUA_UAPI_ERRNO_H
#define PERSPICUA_UAPI_ERRNO_H

#define EPERM        1  // Operation not permitted
#define ENOENT       2  // No such file or directory
#define ESRCH        3  // No such process
#define EINTR        4  // Interrupted system call
#define EIO          5  // I/O error
#define ENXIO        6  // No such device or address
#define E2BIG        7  // Argument list too long
#define ENOEXEC      8  // Exec format error
#define EBADF        9  // Bad file descriptor
#define ECHILD       10 // No child processes
#define EAGAIN       11 // Resource temporarily unavailable
#define ENOMEM       12 // Out of memory
#define EACCES       13 // Permission denied
#define EFAULT       14 // Bad address
#define EBUSY        16 // Device or resource busy
#define EEXIST       17 // File exists
#define EXDEV        18 // Cross-device link
#define ENODEV       19 // No such device
#define ENOTDIR      20 // Not a directory
#define EISDIR       21 // Is a directory
#define EINVAL       22 // Invalid argument
#define ENFILE       23 // Too many open files in system
#define EMFILE       24 // Too many open files
#define ENOTTY       25 // Inappropriate ioctl for device
#define EFBIG        27 // File too large
#define ENOSPC       28 // No space left on device
#define ESPIPE       29 // Illegal seek
#define EROFS        30 // Read-only file system
#define EPIPE        32 // Broken pipe
#define ERANGE       34 // Result too large
#define EDEADLK      35 // Resource deadlock avoided
#define ENAMETOOLONG 36 // File name too long
#define ENOSYS       38 // Function not implemented
#define ENOTEMPTY    39 // Directory not empty
#define ELOOP        40 // Too many levels of symbolic links
#define EWOULDBLOCK  EAGAIN
#define ENOTSUP      95 // Operation not supported
#define EOPNOTSUPP   ENOTSUP
#define ETIMEDOUT    110 // Connection timed out
#define ECONNREFUSED 111 // Connection refused

#endif // PERSPICUA_UAPI_ERRNO_H
