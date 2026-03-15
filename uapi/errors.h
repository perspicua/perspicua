/*
 * errors.h - System-wide error code definitions.
 *
 * This file defines the success and error codes used by both the kernel
 * and userspace for consistent error reporting across the system.
 */

#ifndef PERSPICUA_UAPI_ERRORS_H
#define PERSPICUA_UAPI_ERRORS_H

/* Success code */
#define PERS_SUCCESS 0

/* Standard error codes (returned as negative values) */
#define PERS_ERR_NOT_FOUND               1  /* No such file, directory, or process */
#define PERS_ERR_NOT_A_DIRECTORY         2  /* Component in path is not a directory */
#define PERS_ERR_IS_A_DIRECTORY           3  /* Operation not allowed on a directory */
#define PERS_ERR_PERMISSION_DENIED       4  /* Operation not permitted */
#define PERS_ERR_ALREADY_EXISTS          5  /* Resource already exists */
#define PERS_ERR_OUT_OF_RESOURCES        6  /* System-wide limit reached */
#define PERS_ERR_INVALID_ARGUMENT        7  /* Invalid pointer or value provided */
#define PERS_ERR_OUT_OF_MEMORY           8  /* Allocation failed */
#define PERS_ERR_IO_ERROR                9  /* Hardware or driver failure */
#define PERS_ERR_TRY_AGAIN               10 /* Resource busy, retry later */
#define PERS_ERR_NO_SUCH_PROCESS         11 /* Process ID does not exist */
#define PERS_ERR_NOT_IMPLEMENTED         12 /* Feature not yet supported */
#define PERS_ERR_BUFFER_TOO_SMALL        13 /* Provided buffer is insufficient */
#define PERS_ERR_NOT_A_DEVICE            14 /* Target is not a device node */
#define PERS_ERR_READ_ONLY_FS            15 /* Write attempted on read-only media */
#define PERS_ERR_FILE_TOO_LARGE          16 /* File size limit exceeded */
#define PERS_ERR_NO_SPACE_LEFT           17 /* Storage capacity reached */
#define PERS_ERR_BAD_FILE_DESCRIPTOR     18 /* Invalid or closed file descriptor */
#define PERS_ERR_EXECUTABLE_FORMAT_ERROR 19 /* Invalid executable (e.g., bad ELF) */
#define PERS_ERR_INTERRUPTED             20 /* Operation was canceled by an event */
#define PERS_ERR_RESOURCE_DEADLOCK       21 /* Operation would cause a deadlock */
#define PERS_ERR_BROKEN_PIPE             22 /* Pipe/socket has no reader */
#define PERS_ERR_CONNECTION_REFUSED      23 /* Network/IPC connection denied */
#define PERS_ERR_TIMED_OUT               24 /* Operation exceeded time limit */
#define PERS_ERR_ILLEGAL_SEEK            25 /* Seek not supported on this object */
#define PERS_ERR_NAME_TOO_LONG           26 /* Filename or path exceeds limit */
#define PERS_ERR_DIR_NOT_EMPTY           27 /* Directory contains entries */
#define PERS_ERR_TOO_MANY_SYMLINKS       28 /* Symbolic link loop detected */
#define PERS_ERR_CROSS_DEVICE_LINK       29 /* Link across mount points failed */
#define PERS_ERR_OPERATION_NOT_SUPPORTED 30 /* Unsupported operation for object type */

#define PERS_ERR_UNKNOWN                 99 /* Unspecified internal error */

#endif /* PERSPICUA_UAPI_ERRORS_H */
