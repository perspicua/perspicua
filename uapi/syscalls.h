/*
 * syscalls.h - System call number definitions.
 *
 * This header defines the unique numeric identifiers for each system call
 * shared between the kernel dispatcher and userspace wrappers.
 */

#ifndef PERSPICUA_UAPI_SYSCALLS_H
#define PERSPICUA_UAPI_SYSCALLS_H

/* --- System Call Identifiers --- */

#define SYS_WRITE       1
#define SYS_EXIT        2
#define SYS_GETPID      3
#define SYS_YIELD       4
#define SYS_SLEEP       5
#define SYS_OPEN        6
#define SYS_READ        7
#define SYS_CLOSE       8
#define SYS_EXEC        9
#define SYS_FORK        10
#define SYS_WAITPID     11
#define SYS_PIPE        12
#define SYS_DUP2        13
#define SYS_SIGNAL      14
#define SYS_SIGRETURN   15
#define SYS_KILL        16
#define SYS_SIGRESTORE  17
#define SYS_GETDENTS    18
#define SYS_CHDIR       19
#define SYS_GETCWD      20
#define SYS_MMAP        21
#define SYS_SIGACTION   22
#define SYS_SIGPROCMASK 23
#define SYS_SIGPENDING  24
#define SYS_SIGSUSPEND  25
#define SYS_STAT        26
#define SYS_LSEEK       27

#endif /* PERSPICUA_UAPI_SYSCALLS_H */
