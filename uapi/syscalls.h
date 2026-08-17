/*
 * syscalls.h - System call number definitions.
 *
 * This header defines the unique numeric identifiers for each system call
 * shared between the kernel dispatcher and userspace wrappers.
 */

#ifndef PERSPICUA_UAPI_SYSCALLS_H
#define PERSPICUA_UAPI_SYSCALLS_H

/* --- System Call Identifiers --- */

<<<<<<< Updated upstream
=======
<<<<<<< Updated upstream
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
#define SYS_SYNC        28
#define SYS_MKDIR       29
#define SYS_RMDIR       30
#define SYS_UNLINK      31
#define SYS_RENAME      32
#define SYS_FSYNC       33
#define SYS_FCNTL       34
#define SYS_PREAD       35
#define SYS_PWRITE      36
#define SYS_GETPPID     37
#define SYS_SETPGID     38
#define SYS_GETPGID     39
#define SYS_TCSETPGRP   40
#define SYS_TCGETPGRP   41
#define SYS_SETSID      42
#define SYS_GETSID      43
=======
>>>>>>> Stashed changes
#define SYS_WRITE         1
#define SYS_EXIT          2
#define SYS_GETPID        3
#define SYS_YIELD         4
#define SYS_SLEEP         5
#define SYS_OPEN          6
#define SYS_READ          7
#define SYS_CLOSE         8
#define SYS_EXEC          9
#define SYS_FORK          10
#define SYS_WAITPID       11
#define SYS_PIPE          12
#define SYS_DUP2          13
#define SYS_SIGNAL        14
#define SYS_SIGRETURN     15
#define SYS_KILL          16
#define SYS_SIGRESTORE    17
#define SYS_GETDENTS      18
#define SYS_CHDIR         19
#define SYS_GETCWD        20
#define SYS_MMAP          21
#define SYS_SIGACTION     22
#define SYS_SIGPROCMASK   23
#define SYS_SIGPENDING    24
#define SYS_SIGSUSPEND    25
#define SYS_STAT          26
#define SYS_LSEEK         27
#define SYS_SYNC          28
#define SYS_MKDIR         29
#define SYS_RMDIR         30
#define SYS_UNLINK        31
#define SYS_RENAME        32
#define SYS_FSYNC         33
#define SYS_FCNTL         34
#define SYS_PREAD         35
#define SYS_PWRITE        36
#define SYS_GETPPID       37
#define SYS_SETPGID       38
#define SYS_GETPGID       39
#define SYS_TCSETPGRP     40
#define SYS_TCGETPGRP     41
#define SYS_SETSID        42
#define SYS_GETSID        43
#define SYS_GETTIMEOFDAY  44
#define SYS_CLOCK_GETTIME 45
<<<<<<< Updated upstream
=======
#define SYS_NANOSLEEP     46
>>>>>>> Stashed changes
>>>>>>> Stashed changes

#endif /* PERSPICUA_UAPI_SYSCALLS_H */
