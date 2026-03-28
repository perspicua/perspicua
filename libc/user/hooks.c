/*
 * hooks.c - Userspace libc glue logic.
 *
 * This file implements the output hooks for the user-mode version
 * of libc, routing printf output to the system's write call.
 */

#include "types.h"
#include "syscall.h"

/* --- Public API Implementations --- */

/* Routes string data to the standard output file descriptor. */
void __libc_write(const char *buf, size_t len)
{
    sys_write(1, buf, len);
}
