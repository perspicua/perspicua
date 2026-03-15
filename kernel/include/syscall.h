/*
 * syscall.h - Public API for the system call dispatcher.
 *
 * This file defines the entry point for handling system calls triggered
 * by user-mode applications via the SVC instruction.
 */

#ifndef PERSPICUA_KERNEL_SYSCALL_H
#define PERSPICUA_KERNEL_SYSCALL_H

#include "arch/exception.h"

/*
 * syscall_handle - The central dispatcher for all system calls.
 * Processes the syscall number and arguments from the trap frame and
 * invokes the corresponding kernel subsystem function.
 */
void syscall_handle(struct exception_trap_frame* tf);

#endif /* PERSPICUA_KERNEL_SYSCALL_H */
