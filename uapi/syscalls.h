/*
 * syscalls.h - System call number definitions.
 *
 * This file defines the unique numeric identifiers for each system call
 * provided by the kernel, used by both the userspace wrappers and the
 * kernel dispatcher.
 */

#ifndef PERSPICUA_UAPI_SYSCALLS_H
#define PERSPICUA_UAPI_SYSCALLS_H

/* System call identifiers */
#define SYS_WRITE   1
#define SYS_EXIT    2
#define SYS_GETPID  3
#define SYS_YIELD   4
#define SYS_SLEEP   5
#define SYS_OPEN    6
#define SYS_READ    7
#define SYS_CLOSE   8
#define SYS_EXEC    9
#define SYS_FORK    10
#define SYS_WAITPID 11

#endif /* PERSPICUA_UAPI_SYSCALLS_H */
