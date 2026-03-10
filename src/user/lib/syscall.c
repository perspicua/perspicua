#include "syscall.h"

void sys_exit(void)
{
    asm volatile("mov x8, #2\n svc #0" : : : "x8", "memory");
}

void sys_write(int fd, const char* buf, size_t len)
{
    register int _fd asm("x0") = fd;
    register const char* _buf asm("x1") = buf;
    register size_t _len asm("x2") = len;
    asm volatile("mov x8, #1\n svc #0" : : "r"(_fd), "r"(_buf), "r"(_len) : "x8", "memory");
}

int sys_getpid(void)
{
    register int pid asm("x0");
    asm volatile("mov x8, #3\n svc #0" : "=r"(pid) : : "x8", "memory");
    return pid;
}

void sys_yield(void)
{
    asm volatile("mov x8, #4\n svc #0" : : : "x8", "memory");
}

void sys_sleep(unsigned long ms)
{
    register unsigned long _ms asm("x0") = ms;
    asm volatile("mov x8, #5\n svc #0" : : "r"(_ms) : "x8", "memory");
}

int sys_open(const char* path, int flags)
{
    register const char* _path asm("x0") = path;
    register int _flags asm("x1") = flags;
    register int fd asm("x0");
    asm volatile("mov x8, #6\n svc #0" : "=r"(fd) : "r"(_path), "r"(_flags) : "x8", "memory");
    return fd;
}

int sys_read(int fd, void* buf, size_t len)
{
    register int _fd asm("x0") = fd;
    register void* _buf asm("x1") = buf;
    register size_t _len asm("x2") = len;
    register int bytes asm("x0");
    asm volatile("mov x8, #7\n svc #0" : "=r"(bytes) : "r"(_fd), "r"(_buf), "r"(_len) : "x8", "memory");
    return bytes;
}

int sys_close(int fd)
{
    register int _fd asm("x0") = fd;
    register int res asm("x0");
    asm volatile("mov x8, #8\n svc #0" : "=r"(res) : "r"(_fd) : "x8", "memory");
    return res;
}

int sys_exec(const char* path)
{
    register const char* _path asm("x0") = path;
    register int res asm("x0");
    asm volatile("mov x8, #9\n svc #0" : "=r"(res) : "r"(_path) : "x8", "memory");
    return res;
}
