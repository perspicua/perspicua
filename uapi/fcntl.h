/*
 * fcntl.h - File open flags, fcntl commands, and seek origins.
 *
 * Shared by the kernel and userspace: these values cross the syscall boundary,
 * so both sides must agree on them and there is only one place to change them.
 */

#ifndef PERSPICUA_UAPI_FCNTL_H
#define PERSPICUA_UAPI_FCNTL_H

// Access mode: the low two bits of the open() flags word.
#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_ACCMODE 0x0003

#define O_CREAT    0x0100
#define O_TRUNC    0x0200
#define O_APPEND   0x0400
#define O_CLOEXEC  0x0800
#define O_NONBLOCK 0x1000

// fcntl commands
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4

// fcntl file descriptor flags
#define FD_CLOEXEC 1

// lseek origins
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#endif // PERSPICUA_UAPI_FCNTL_H
