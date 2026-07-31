/*
 * syscall.c - Userspace system call wrapper implementations.
 *
 * This file provides the AArch64 assembly wrappers for the libc API,
 * using the SVC instruction to transition to the kernel.
 *
 * Each wrapper translates the kernel's negative PERS_ERR_* return value
 * into a standard POSIX errno code and returns -1 (or the appropriate
 * sentinel) on failure, matching the POSIX syscall contract.
 */

#include "syscall.h"
#include "errno.h"

#include "uapi/syscalls.h"
#include "uapi/errors.h"
#include "uapi/mman.h"

/* --- Internal errno translation --- */

/*
 * Map a PERS_ERR_* code to the corresponding POSIX errno value.
 * Called with the positive error code extracted from a negative return.
 */
static int __pers_to_errno(int pers_err)
{
    switch (pers_err) {
        case PERS_ERR_NOT_FOUND:
            return ENOENT;
        case PERS_ERR_NOT_A_DIRECTORY:
            return ENOTDIR;
        case PERS_ERR_IS_A_DIRECTORY:
            return EISDIR;
        case PERS_ERR_PERMISSION_DENIED:
            return EACCES;
        case PERS_ERR_ALREADY_EXISTS:
            return EEXIST;
        case PERS_ERR_OUT_OF_RESOURCES:
            return ENFILE;
        case PERS_ERR_INVALID_ARGUMENT:
            return EINVAL;
        case PERS_ERR_OUT_OF_MEMORY:
            return ENOMEM;
        case PERS_ERR_IO_ERROR:
            return EIO;
        case PERS_ERR_TRY_AGAIN:
            return EAGAIN;
        case PERS_ERR_NO_SUCH_PROCESS:
            return ESRCH;
        case PERS_ERR_NOT_IMPLEMENTED:
            return ENOSYS;
        case PERS_ERR_BUFFER_TOO_SMALL:
            return ERANGE;
        case PERS_ERR_NOT_A_DEVICE:
            return ENODEV;
        case PERS_ERR_READ_ONLY_FS:
            return EROFS;
        case PERS_ERR_FILE_TOO_LARGE:
            return EFBIG;
        case PERS_ERR_NO_SPACE_LEFT:
            return ENOSPC;
        case PERS_ERR_BAD_FILE_DESCRIPTOR:
            return EBADF;
        case PERS_ERR_EXECUTABLE_FORMAT_ERROR:
            return ENOEXEC;
        case PERS_ERR_INTERRUPTED:
            return EINTR;
        case PERS_ERR_RESOURCE_DEADLOCK:
            return EDEADLK;
        case PERS_ERR_BROKEN_PIPE:
            return EPIPE;
        case PERS_ERR_CONNECTION_REFUSED:
            return ECONNREFUSED;
        case PERS_ERR_TIMED_OUT:
            return ETIMEDOUT;
        case PERS_ERR_ILLEGAL_SEEK:
            return ESPIPE;
        case PERS_ERR_NAME_TOO_LONG:
            return ENAMETOOLONG;
        case PERS_ERR_DIR_NOT_EMPTY:
            return ENOTEMPTY;
        case PERS_ERR_TOO_MANY_SYMLINKS:
            return ELOOP;
        case PERS_ERR_CROSS_DEVICE_LINK:
            return EXDEV;
        case PERS_ERR_OPERATION_NOT_SUPPORTED:
            return ENOTSUP;
        default:
            return EIO;
    }
}

/* Set errno from a raw kernel return value and return -1. */
static inline int __set_errno_ret(long res)
{
    errno = __pers_to_errno((int)-res);
    return -1;
}

/* Translate a raw kernel return to a POSIX int result. */
static inline int __syscall_ret(long res)
{
    if (res < 0)
        return __set_errno_ret(res);
    return (int)res;
}

/* Translate a raw kernel return to a POSIX off_t result. */
static inline off_t __syscall_off_ret(long res)
{
    if (res < 0) {
        errno = __pers_to_errno((int)-res);
        return (off_t)-1;
    }
    return (off_t)res;
}

/* Translate a raw kernel return to a POSIX mmap result. */
static inline void *__syscall_mmap_ret(long res)
{
    if (res < 0) {
        errno = __pers_to_errno((int)-res);
        return MAP_FAILED;
    }
    return (void *)res;
}

/* --- Public API Implementations --- */

void sys_exit(int status)
{
    asm volatile("mov x0, %0\n"
                 "mov x8, %1\n"
                 "svc #0"
                 :
                 : "r"((long)status), "i"(SYS_EXIT)
                 : "x0", "x8", "memory");
    __builtin_unreachable();
}

int sys_write(int fd, const char *buf, size_t len)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x2, %3\n"
                 "mov x8, %4\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"((long)fd), "r"(buf), "r"((long)len), "i"(SYS_WRITE)
                 : "x0", "x1", "x2", "x8", "memory");
    return __syscall_ret(res);
}

int sys_pwrite(int fd, const char *buf, size_t len, off_t offset)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x2, %3\n"
                 "mov x3, %4\n"
                 "mov x8, %5\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"((long)fd), "r"(buf), "r"((long)len), "r"((long)offset), "i"(SYS_PWRITE)
                 : "x0", "x1", "x2", "x3", "x8", "memory");
    return __syscall_ret(res);
}

int sys_getpid(void)
{
    long pid;
    asm volatile("mov x8, %1\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(pid)
                 : "i"(SYS_GETPID)
                 : "x0", "x8", "memory");
    return __syscall_ret(pid);
}

int sys_getppid(void)
{
    long ppid;
    asm volatile("mov x8, %1\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(ppid)
                 : "i"(SYS_GETPPID)
                 : "x0", "x8", "memory");
    return __syscall_ret(ppid);
}

void sys_yield(void)
{
    asm volatile("mov x8, %0\n"
                 "svc #0"
                 :
                 : "i"(SYS_YIELD)
                 : "x8", "memory");
}

void sys_sleep(unsigned long ms)
{
    asm volatile("mov x0, %0\n"
                 "mov x8, %1\n"
                 "svc #0"
                 :
                 : "r"(ms), "i"(SYS_SLEEP)
                 : "x0", "x8", "memory");
}

int sys_open(const char *path, int flags)
{
    long fd;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x8, %3\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(fd)
                 : "r"(path), "r"((long)flags), "i"(SYS_OPEN)
                 : "x0", "x1", "x8", "memory");
    return __syscall_ret(fd);
}

int sys_read(int fd, void *buf, size_t len)
{
    long bytes;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x2, %3\n"
                 "mov x8, %4\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(bytes)
                 : "r"((long)fd), "r"(buf), "r"((long)len), "i"(SYS_READ)
                 : "x0", "x1", "x2", "x8", "memory");
    return __syscall_ret(bytes);
}

int sys_pread(int fd, void *buf, size_t count, off_t offset)
{
    long bytes;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x2, %3\n"
                 "mov x3, %4\n"
                 "mov x8, %5\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(bytes)
                 : "r"((long)fd), "r"(buf), "r"((long)count), "r"((long)offset), "i"(SYS_PREAD)
                 : "x0", "x1", "x2", "x3", "x8", "memory");
    return __syscall_ret(bytes);
}

int sys_getdents(int fd, void *buf, size_t count)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x2, %3\n"
                 "mov x8, %4\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"((long)fd), "r"(buf), "r"((long)count), "i"(SYS_GETDENTS)
                 : "x0", "x1", "x2", "x8", "memory");
    return __syscall_ret(res);
}

int sys_close(int fd)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x8, %2\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"((long)fd), "i"(SYS_CLOSE)
                 : "x0", "x8", "memory");
    return __syscall_ret(res);
}

int sys_exec(const char *path, char *const argv[], char *const envp[])
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x2, %3\n"
                 "mov x8, %4\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"(path), "r"(argv), "r"(envp), "i"(SYS_EXEC)
                 : "x0", "x1", "x2", "x8", "memory");
    return __syscall_ret(res);
}

int sys_fork(void)
{
    long res;
    asm volatile("mov x8, %1\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "i"(SYS_FORK)
                 : "x0", "x8", "memory");
    return __syscall_ret(res);
}

int sys_waitpid(int pid, int *status, int options)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x2, %3\n"
                 "mov x8, %4\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"((long)pid), "r"(status), "r"((long)options), "i"(SYS_WAITPID)
                 : "x0", "x1", "x2", "x8", "memory");
    return __syscall_ret(res);
}

int sys_pipe(int pipefd[2])
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x8, %2\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"(pipefd), "i"(SYS_PIPE)
                 : "x0", "x8", "memory");
    return __syscall_ret(res);
}

int sys_dup2(int oldfd, int newfd)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x8, %3\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"((long)oldfd), "r"((long)newfd), "i"(SYS_DUP2)
                 : "x0", "x1", "x8", "memory");
    return __syscall_ret(res);
}

int sys_signal(int sig, signal_handler_t handler)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x8, %3\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"((long)sig), "r"(handler), "i"(SYS_SIGNAL)
                 : "x0", "x1", "x8", "memory");
    return __syscall_ret(res);
}

int sys_kill(int pid, int sig)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x8, %3\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"((long)pid), "r"((long)sig), "i"(SYS_KILL)
                 : "x0", "x1", "x8", "memory");
    return __syscall_ret(res);
}

void sys_sigreturn(void)
{
    asm volatile("mov x8, %0\n"
                 "svc #0"
                 :
                 : "i"(SYS_SIGRETURN)
                 : "x8", "memory");
}

void sys_sigrestore(uintptr_t restorer)
{
    asm volatile("mov x0, %0\n"
                 "mov x8, %1\n"
                 "svc #0"
                 :
                 : "r"(restorer), "i"(SYS_SIGRESTORE)
                 : "x0", "x8", "memory");
}

int sys_sigaction(int sig, const struct sigaction *act, struct sigaction *oact)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x2, %3\n"
                 "mov x8, %4\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"((long)sig), "r"(act), "r"(oact), "i"(SYS_SIGACTION)
                 : "x0", "x1", "x2", "x8", "memory");
    return __syscall_ret(res);
}

int sys_sigprocmask(int how, const sigset_t *set, sigset_t *oset)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x2, %3\n"
                 "mov x8, %4\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"((long)how), "r"(set), "r"(oset), "i"(SYS_SIGPROCMASK)
                 : "x0", "x1", "x2", "x8", "memory");
    return __syscall_ret(res);
}

int sys_sigpending(sigset_t *set)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x8, %2\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"(set), "i"(SYS_SIGPENDING)
                 : "x0", "x8", "memory");
    return __syscall_ret(res);
}

int sys_sigsuspend(const sigset_t *mask)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x8, %2\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"(mask), "i"(SYS_SIGSUSPEND)
                 : "x0", "x8", "memory");
    return __syscall_ret(res);
}

int sys_chdir(const char *path)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x8, %2\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"(path), "i"(SYS_CHDIR)
                 : "x0", "x8", "memory");
    return __syscall_ret(res);
}

int sys_getcwd(char *buf, size_t size)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x8, %3\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"(buf), "r"((long)size), "i"(SYS_GETCWD)
                 : "x0", "x1", "x8", "memory");
    return __syscall_ret(res);
}

int sys_stat(const char *path, struct stat *buf)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x8, %3\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"(path), "r"(buf), "i"(SYS_STAT)
                 : "x0", "x1", "x8", "memory");
    return __syscall_ret(res);
}

void *sys_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x2, %3\n"
                 "mov x3, %4\n"
                 "mov x4, %5\n"
                 "mov x5, %6\n"
                 "mov x8, %7\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"(addr), "r"(length), "r"((long)prot), "r"((long)flags), "r"((long)fd),
                   "r"(offset), "i"(SYS_MMAP)
                 : "x0", "x1", "x2", "x3", "x4", "x5", "x8", "memory");
    return __syscall_mmap_ret(res);
}

off_t sys_lseek(int fd, off_t offset, int whence)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x2, %3\n"
                 "mov x8, %4\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"((long)fd), "r"((long)offset), "r"((long)whence), "i"(SYS_LSEEK)
                 : "x0", "x1", "x2", "x8", "memory");
    return __syscall_off_ret(res);
}

int sys_sync(void)
{
    long res;
    asm volatile("mov x8, %1\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "i"(SYS_SYNC)
                 : "x0", "x8", "memory");
    return __syscall_ret(res);
}

int sys_mkdir(const char *path, int mode)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x8, %3\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"(path), "r"((long)mode), "i"(SYS_MKDIR)
                 : "x0", "x1", "x8", "memory");
    return __syscall_ret(res);
}

int sys_rmdir(const char *path)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x8, %2\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"(path), "i"(SYS_RMDIR)
                 : "x0", "x1", "x8", "memory");
    return __syscall_ret(res);
}

int sys_unlink(const char *path)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x8, %2\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"(path), "i"(SYS_UNLINK)
                 : "x0", "x8", "memory");
    return __syscall_ret(res);
}

int sys_rename(const char *oldpath, const char *newpath)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x8, %3\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"(oldpath), "r"(newpath), "i"(SYS_RENAME)
                 : "x0", "x1", "x8", "memory");
    return __syscall_ret(res);
}

int sys_fsync(int fd)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x8, %2\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"((long)fd), "i"(SYS_FSYNC)
                 : "x0", "x8", "memory");
    return __syscall_ret(res);
}

int sys_fcntl(int fd, int cmd, int arg)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x2, %3\n"
                 "mov x8, %4\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"((long)fd), "r"((long)cmd), "r"((long)arg), "i"(SYS_FCNTL)
                 : "x0", "x1", "x2", "x8", "memory");
    return __syscall_ret(res);
}

int sys_setpgid(int pid, int pgid)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x8, %3\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"((long)pid), "r"((long)pgid), "i"(SYS_SETPGID)
                 : "x0", "x1", "x8", "memory");
    return __syscall_ret(res);
}

int sys_getpgid(int pid)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x8, %2\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"((long)pid), "i"(SYS_GETPGID)
                 : "x0", "x8", "memory");
    return __syscall_ret(res);
}

int sys_tcsetpgrp(int fd, int pgid)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x8, %3\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"((long)fd), "r"((long)pgid), "i"(SYS_TCSETPGRP)
                 : "x0", "x1", "x8", "memory");
    return __syscall_ret(res);
}

int sys_tcgetpgrp(int fd)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x8, %2\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"((long)fd), "i"(SYS_TCGETPGRP)
                 : "x0", "x8", "memory");
    return __syscall_ret(res);
}

int sys_setsid(void)
{
    long res;
    asm volatile("mov x8, %1\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "i"(SYS_SETSID)
                 : "x0", "x8", "memory");
    return __syscall_ret(res);
}

int sys_getsid(int pid)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x8, %2\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"((long)pid), "i"(SYS_GETSID)
                 : "x0", "x8", "memory");
    return __syscall_ret(res);
}
