/*
 * syscall.h - Public API for the system call dispatcher.
 */

#ifndef PERSPICUA_CORE_SYSCALL_H
#define PERSPICUA_CORE_SYSCALL_H

#include <stddef.h>

#include "arch/exception.h"
#include "uapi/syscalls.h"

#define SYSCALL_MAX_MMAP_SIZE (256UL * 1024 * 1024)

int syscall_validate_user_buffer(const void *ptr, size_t len, int writable);

void syscall_handle(struct exception_trap_frame *tf);

#ifdef CONFIG_TESTS
// Set to make the next result copied out to user memory fail, as a mapping changed under it would.
extern volatile int syscall_test_fail_copy_out;
#endif

#endif // PERSPICUA_CORE_SYSCALL_H
