/*
 * syscall.h - Userspace system call wrapper definitions.
 *
 * This header provides the C interface for system calls available to
 * user-mode applications.
 */

#ifndef PERSPICUA_LIBC_SYSCALL_H
#define PERSPICUA_LIBC_SYSCALL_H

/*
 * Guard the userspace/kernel boundary at compile time. Without this, a
 * syscall-dependent source added to LIBC_SRC_COMMON still compiles into libk
 * and only fails at the unrelated, much later link that first references it.
 * Kernel code dispatches syscalls through core/syscall.h instead.
 */
#ifdef __KERNEL__
    #error "syscall.h is userspace-only; kernel code uses core/syscall.h"
#endif

#include "types.h"
#include "signals.h"

#include "uapi/time.h"
#include "uapi/stat.h"

/* Filesystem access mode and control flags. */
#define VFS_O_RDONLY  0x0000
#define VFS_O_WRONLY  0x0001
#define VFS_O_RDWR    0x0002
#define VFS_O_ACCMODE 0x0003

#define VFS_O_CREAT    0x0100
#define VFS_O_TRUNC    0x0200
#define VFS_O_APPEND   0x0400
#define VFS_O_CLOEXEC  0x0800
#define VFS_O_NONBLOCK 0x1000

/* fcntl commands */
#define VFS_F_GETFD 1
#define VFS_F_SETFD 2
#define VFS_F_GETFL 3
#define VFS_F_SETFL 4

/* fcntl file descriptor flags */
#define VFS_FD_CLOEXEC 1

/* Seek mode constants. */
#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

/* Directory entry structure returned to userspace. */
struct vfs_dirent {
    uint32_t ino;
    char name[256];
};

/* Process and execution control */
__attribute__((noreturn)) void sys_exit(int status);
int sys_getpid(void);
int sys_getppid(void);
int sys_setpgid(int pid, int pgid);
int sys_getpgid(int pid);
int sys_setsid(void);
int sys_getsid(int pid);
int sys_tcsetpgrp(int fd, int pgid);
int sys_tcgetpgrp(int fd);
void sys_yield(void);
void sys_sleep(unsigned long ms);
int sys_exec(const char *path, char *const argv[], char *const envp[]);
int sys_fork(void);
int sys_waitpid(int pid, int *status, int options);

/* Filesystem and I/O */
int sys_open(const char *path, int flags);
int sys_close(int fd);
int sys_read(int fd, void *buf, size_t len);
int sys_pread(int fd, void *buf, size_t count, off_t offset);
int sys_write(int fd, const char *buf, size_t len);
int sys_pwrite(int fd, const char *buf, size_t len, off_t offset);
int sys_getdents(int fd, void *buf, size_t count);
int sys_pipe(int pipefd[2]);
int sys_dup2(int oldfd, int newfd);
int sys_chdir(const char *path);
int sys_getcwd(char *buf, size_t size);
int sys_stat(const char *path, struct stat *buf);
off_t sys_lseek(int fd, off_t offset, int whence);
int sys_mkdir(const char *path, int mode);
int sys_rmdir(const char *path);
int sys_unlink(const char *path);
int sys_rename(const char *oldpath, const char *newpath);
int sys_fcntl(int fd, int cmd, int arg);

/* Memory management */
void *sys_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);

/* Filesystem sync */
int sys_sync(void);
int sys_fsync(int fd);

/* Signal handling */
int sys_signal(int sig, signal_handler_t handler);
int sys_kill(int pid, int sig);
void sys_sigreturn(void);
void sys_sigrestore(uintptr_t restorer);
int sys_sigaction(int sig, const struct sigaction *act, struct sigaction *oact);
int sys_sigprocmask(int how, const sigset_t *set, sigset_t *oset);
int sys_sigpending(sigset_t *set);
int sys_sigsuspend(const sigset_t *mask);

/* Time */
int sys_gettimeofday(struct timeval *tv, void *tz);
int sys_clock_gettime(clockid_t clk_id, struct timespec *tp);

#endif /* PERSPICUA_LIBC_SYSCALL_H */
