/*
 * hooks.c - Userspace libc glue logic.
 */

#include "types.h"
#include "syscall.h"

// Public API Implementations

// Routes string data to the standard output file descriptor.
void __libc_write(const char *buf, size_t len)
{
    sys_write(1, buf, len);
}
