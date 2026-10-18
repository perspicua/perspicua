/*
 * hooks.c - Userspace libc glue logic.
 */

#include "libc/internal.h"

#include <stddef.h>

#include "stdio.h"

// Routes printf's output into stdout, which buffers it when stdout is not a terminal.
void __libc_write(const char *buf, size_t len)
{
    fwrite(buf, 1, len, stdout);
}
