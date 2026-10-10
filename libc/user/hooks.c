/*
 * hooks.c - Userspace libc glue logic.
 */

#include <stddef.h>
#include "stdio.h"

// Public API Implementations

// Routes printf's output into stdout, which buffers it when stdout is not a terminal.
void __libc_write(const char *buf, size_t len)
{
    fwrite(buf, 1, len, stdout);
}
