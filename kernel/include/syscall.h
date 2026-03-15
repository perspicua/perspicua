#ifndef _SYSCALL_H_
#define _SYSCALL_H_

#include "arch/exception.h"

void handle_syscall(struct exception_trap_frame* tf);

#endif // _SYSCALL_H_
