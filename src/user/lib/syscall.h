#ifndef _LIBUSER_SYSCALL_H_
#define _LIBUSER_SYSCALL_H_

#include "lib/types.h"

void sys_write(const char* buf, size_t len);
void sys_exit(void);
int sys_getpid(void);
void sys_yield(void);
void sys_sleep(unsigned long ms);

#endif // _LIBUSER_SYSCALL_H_
