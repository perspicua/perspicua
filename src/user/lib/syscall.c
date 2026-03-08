#include "syscall.h"

void sys_exit(void)
{
    asm volatile("mov x8, #2\n svc #0" : : : "x8");
}

void sys_write(const char* buf, size_t len)
{
    register const char* _buf asm("x0") = buf;
    register size_t _len asm("x1") = len;
    asm volatile("mov x8, #1\n svc #0" : : "r"(_buf), "r"(_len) : "x8");
}

int sys_getpid(void)
{
    register int pid asm("x0");
    asm volatile("mov x8, #3\n svc #0" : "=r"(pid) : : "x8");
    return pid;
}

void sys_yield(void)
{
    asm volatile("mov x8, #4\n svc #0" : : : "x8");
}

void sys_sleep(unsigned long ms)
{
    register unsigned long _ms asm("x0") = ms;
    asm volatile("mov x8, #5\n svc #0" : : "r"(_ms) : "x8");
}
