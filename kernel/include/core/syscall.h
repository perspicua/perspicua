/*
 * syscall.h - Public API for the system call dispatcher.
 */

#ifndef PERSPICUA_CORE_SYSCALL_H
#define PERSPICUA_CORE_SYSCALL_H

#include "arch/exception.h"

#define SYSCALL_MAX_RW_SIZE   (4UL * 1024 * 1024)
#define SYSCALL_MAX_MMAP_SIZE (256UL * 1024 * 1024)

int validate_user_buffer(const void *ptr, size_t len, int writable);

void syscall_handle(struct exception_trap_frame *tf);

#endif // PERSPICUA_CORE_SYSCALL_H
