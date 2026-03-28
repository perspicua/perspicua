/*
 * syscall.h - Public API for the system call dispatcher.
 *
 * This header defines the entry point for handling system calls triggered
 * by user-mode applications via the SVC instruction.
 */

#ifndef PERSPICUA_CORE_SYSCALL_H
#define PERSPICUA_CORE_SYSCALL_H

#include "arch/exception.h"

#define SYSCALL_MAX_RW_SIZE   (4UL * 1024 * 1024)
#define SYSCALL_MAX_MMAP_SIZE (256UL * 1024 * 1024)

/*
 * validate_user_buffer - Verifies that a memory range is accessible from user mode.
 */
int validate_user_buffer(const void *ptr, size_t len, int writable);

/*
 * syscall_handle - The central dispatcher for all system calls.
 */
void syscall_handle(struct exception_trap_frame *tf);

#endif /* PERSPICUA_CORE_SYSCALL_H */
