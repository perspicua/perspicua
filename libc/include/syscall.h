/*
 * syscall.h - Userspace system call wrapper definitions.
 *
 * This header provides the C interface for system calls available to
 * user-mode applications.
 */

#ifndef PERSPICUA_LIBC_SYSCALL_H
#define PERSPICUA_LIBC_SYSCALL_H

#include "types.h"
#include "signals.h"

#include "uapi/stat.h"

/* Filesystem access mode and control flags. */
#define VFS_O_RDONLY  0x0000
#define VFS_O_WRONLY  0x0001
#define VFS_O_RDWR    0x0002
#define VFS_O_ACCMODE 0x0003

#define VFS_O_CREAT  0x0100
#define VFS_O_TRUNC  0x0200
#define VFS_O_APPEND 0x0400

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
void sys_yield(void);
void sys_sleep(unsigned long ms);
int sys_exec(const char *path, char *const argv[], char *const envp[]);
int sys_fork(void);
int sys_waitpid(int pid, int *status, int options);

/* Filesystem and I/O */
int sys_open(const char *path, int flags);
int sys_close(int fd);
int sys_read(int fd, void *buf, size_t len);
int sys_write(int fd, const char *buf, size_t len);
int sys_getdents(int fd, void *buf, size_t count);
int sys_pipe(int pipefd[2]);
int sys_dup2(int oldfd, int newfd);
int sys_chdir(const char *path);
int sys_getcwd(char *buf, size_t size);
int sys_stat(const char *path, struct stat *buf);
off_t sys_lseek(int fd, off_t offset, int whence);

/* Memory management */
void *sys_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);

/* Filesystem sync */
int sys_sync(void);

/* Signal handling */
int sys_signal(int sig, signal_handler_t handler);
int sys_kill(int pid, int sig);
void sys_sigreturn(void);
void sys_sigrestore(uintptr_t restorer);
int sys_sigaction(int sig, const struct sigaction *act, struct sigaction *oact);
int sys_sigprocmask(int how, const sigset_t *set, sigset_t *oset);
int sys_sigpending(sigset_t *set);
int sys_sigsuspend(const sigset_t *mask);

#endif /* PERSPICUA_LIBC_SYSCALL_H */
