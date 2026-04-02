/*
 * syscall.c - Userspace system call wrapper implementations.
 *
 * This file provides the AArch64 assembly wrappers for the libc API,
 * using the SVC instruction to transition to the kernel.
 */

#include "syscall.h"

#include "uapi/syscalls.h"

/* --- Public API Implementations --- */

void sys_exit(int status)
{
    asm volatile("mov x0, %0\n"
                 "mov x8, %1\n"
                 "svc #0"
                 :
                 : "r"((long)status), "i"(SYS_EXIT)
                 : "x0", "x8", "memory");
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
    return (int)res;
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
    return (int)pid;
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
    return (int)fd;
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
    return (int)bytes;
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
    return (int)res;
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
    return (int)res;
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
    return (int)res;
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
    return (int)res;
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
    return (int)res;
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
    return (int)res;
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
    return (int)res;
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
    return (int)res;
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
    return (int)res;
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
    return (int)res;
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
    return (int)res;
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
    return (int)res;
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
    return (int)res;
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
    return (int)res;
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
    return (int)res;
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
    return (int)res;
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
    return (void *)res;
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
    return (off_t)res;
}
