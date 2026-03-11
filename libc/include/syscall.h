#ifndef _LIBUSER_SYSCALL_H_
#define _LIBUSER_SYSCALL_H_

#include <types.h>

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002
#define O_ACCMODE 0x0003

#define O_CREAT 0x0100
#define O_TRUNC 0x0200
#define O_APPEND 0x0400

void sys_write(int fd, const char* buf, size_t len);
void sys_exit(void);
int sys_getpid(void);
void sys_yield(void);
void sys_sleep(unsigned long ms);
int sys_open(const char* path, int flags);
int sys_read(int fd, void* buf, size_t len);
int sys_close(int fd);
int sys_exec(const char* path);

#endif // _LIBUSER_SYSCALL_H_
