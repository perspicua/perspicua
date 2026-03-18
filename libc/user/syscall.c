/*
 * syscall.c - Userspace system call wrapper implementations.
 *
 * This file contains the AArch64 assembly wrappers for the system calls
 * defined in the libc API, using the SVC instruction to transition to EL1.
 */

#include "syscall.h"

#include "uapi/syscalls.h"

/*
 * sys_exit - Terminates the current process with an exit status.
 */
void sys_exit(int status)
{
    asm volatile("mov x0, %0\n"
                 "mov x8, %1\n"
                 "svc #0"
                 :
                 : "r"((long)status), "i"(SYS_EXIT)
                 : "x0", "x8", "memory");
}

/*
 * sys_write - Writes data to a file descriptor.
 */
void sys_write(int fd, const char* buf, size_t len)
{
    asm volatile("mov x0, %0\n"
                 "mov x1, %1\n"
                 "mov x2, %2\n"
                 "mov x8, %3\n"
                 "svc #0"
                 :
                 : "r"((long)fd), "r"(buf), "r"((long)len), "i"(SYS_WRITE)
                 : "x0", "x1", "x2", "x8", "memory");
}

/*
 * sys_getpid - Retrieves the current process identifier.
 */
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

/*
 * sys_yield - Voluntarily yields the CPU to the scheduler.
 */
void sys_yield(void)
{
    asm volatile("mov x8, %0\n"
                 "svc #0"
                 :
                 : "i"(SYS_YIELD)
                 : "x8", "memory");
}

/*
 * sys_sleep - Puts the current process to sleep for a number of milliseconds.
 */
void sys_sleep(unsigned long ms)
{
    asm volatile("mov x0, %0\n"
                 "mov x8, %1\n"
                 "svc #0"
                 :
                 : "r"(ms), "i"(SYS_SLEEP)
                 : "x0", "x8", "memory");
}

/*
 * sys_open - Opens a file and returns a file descriptor.
 */
int sys_open(const char* path, int flags)
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

/*
 * sys_read - Reads data from a file descriptor.
 */
int sys_read(int fd, void* buf, size_t len)
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

/*
 * sys_getdents - Reads directory entries from a directory file descriptor.
 */
int sys_getdents(int fd, void* buf, size_t count)
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

/*
 * sys_close - Closes an open file descriptor.
 */
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

/*
 * sys_exec - Replaces the current process image with a new executable.
 */
int sys_exec(const char* path)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x8, %2\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"(path), "i"(SYS_EXEC)
                 : "x0", "x8", "memory");
    return (int)res;
}

/*
 * sys_fork - Creates a duplicate of the current process.
 */
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

/*
 * sys_waitpid - Waits for a specific child process to terminate.
 */
int sys_waitpid(int pid, int* status)
{
    long res;
    asm volatile("mov x0, %1\n"
                 "mov x1, %2\n"
                 "mov x8, %3\n"
                 "svc #0\n"
                 "mov %0, x0"
                 : "=r"(res)
                 : "r"((long)pid), "r"(status), "i"(SYS_WAITPID)
                 : "x0", "x1", "x8", "memory");
    return (int)res;
}

/*
 * sys_pipe - Creates an anonymous pipe.
 */
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

/*
 * sys_dup2 - Duplicates a file descriptor to a specific new descriptor.
 */
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

/*
 * sys_signal - Sets the handler for a specific signal.
 */
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

/*
 * sys_kill - Sends a signal to a process.
 */
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

/*
 * sys_sigreturn - Returns from a signal handler, restoring the context.
 */
void sys_sigreturn(void)
{
    asm volatile("mov x8, %0\n"
                 "svc #0"
                 :
                 : "i"(SYS_SIGRETURN)
                 : "x8", "memory");
}

/*
 * sys_sigrestore - Registers the restorer for a signal handler.
 */
void sys_sigrestore(uintptr_t restorer)
{
    asm volatile("mov x0, %0\n"
                 "mov x8, %1\n"
                 "svc #0"
                 :
                 : "r"(restorer), "i"(SYS_SIGRESTORE)
                 : "x0", "x8", "memory");
}

int sys_chdir(const char* path)
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
