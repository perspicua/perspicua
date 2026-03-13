#ifndef _UAPI_ERRORS_H_
#define _UAPI_ERRORS_H_

/*
 * Perspicua OS Error Codes
 * Descriptive, non-standard names for system-wide error reporting.
 * Kernel functions should return these as negative integers (e.g., -PERS_ERR_NOT_FOUND).
 */

#define PERS_SUCCESS 0
#define PERS_ERR_NOT_FOUND 1                /* No such file, directory, or process */
#define PERS_ERR_NOT_A_DIRECTORY 2          /* Component in path is not a directory */
#define PERS_ERR_IS_A_DIRECTORY 3           /* Operation not allowed on a directory (e.g. write) */
#define PERS_ERR_PERMISSION_DENIED 4        /* Operation not permitted for this user/process */
#define PERS_ERR_ALREADY_EXISTS 5           /* File, directory, or resource already exists */
#define PERS_ERR_OUT_OF_RESOURCES 6         /* System-wide limit reached (files, mounts, etc) */
#define PERS_ERR_INVALID_ARGUMENT 7         /* Bad pointer, flag, or out-of-range value */
#define PERS_ERR_OUT_OF_MEMORY 8            /* Kernel heap or slab allocator failed */
#define PERS_ERR_IO_ERROR 9                 /* Hardware or driver-level failure */
#define PERS_ERR_TRY_AGAIN 10               /* Resource busy/locked, operation might succeed later */
#define PERS_ERR_NO_SUCH_PROCESS 11         /* Process ID (PID) does not exist */
#define PERS_ERR_NOT_IMPLEMENTED 12         /* System call or feature is not yet built */
#define PERS_ERR_BUFFER_TOO_SMALL 13        /* Provided buffer is too small for the data */
#define PERS_ERR_NOT_A_DEVICE 14            /* Target is not a device node */
#define PERS_ERR_READ_ONLY_FS 15            /* Attempted write on a read-only filesystem */
#define PERS_ERR_FILE_TOO_LARGE 16          /* File size exceeds system or filesystem limit */
#define PERS_ERR_NO_SPACE_LEFT 17           /* Storage device is full */
#define PERS_ERR_BAD_FILE_DESCRIPTOR 18     /* File descriptor is invalid or not open */
#define PERS_ERR_EXECUTABLE_FORMAT_ERROR 19 /* File is not a valid executable (e.g. bad ELF) */
#define PERS_ERR_INTERRUPTED 20             /* Operation interrupted by a signal or event */
#define PERS_ERR_RESOURCE_DEADLOCK 21       /* Operation would cause a deadlock */
#define PERS_ERR_BROKEN_PIPE 22             /* Writing to a pipe/socket with no reader */
#define PERS_ERR_CONNECTION_REFUSED 23      /* Network/IPC connection refused */
#define PERS_ERR_TIMED_OUT 24               /* Operation took too long and timed out */
#define PERS_ERR_ILLEGAL_SEEK 25            /* Cannot seek on this type of file (e.g. a pipe) */
#define PERS_ERR_NAME_TOO_LONG 26           /* Filename or path exceeds limit */
#define PERS_ERR_DIR_NOT_EMPTY 27           /* Cannot delete a directory that still has files */
#define PERS_ERR_TOO_MANY_SYMLINKS 28       /* Too many levels of symbolic links */
#define PERS_ERR_CROSS_DEVICE_LINK 29       /* Cannot link files across different mount points */
#define PERS_ERR_OPERATION_NOT_SUPPORTED 30 /* The specific operation is not valid for this object */

#define PERS_ERR_UNKNOWN 99 /* Unspecified internal kernel error */

#endif // _UAPI_ERRORS_H_
