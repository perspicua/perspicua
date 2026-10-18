/*
 * internal.h - libc's private hooks: printf's output sink, and the setup crt0 runs before main.
 */

#ifndef PERSPICUA_LIBC_INTERNAL_H
#define PERSPICUA_LIBC_INTERNAL_H

#include <stddef.h>

// The UART in the kernel, stdout in userspace.
void __libc_write(const char *buf, size_t len);

#ifndef __KERNEL__
void __libc_init(int argc, char *argv[], char *envp[]);
#endif

#endif // PERSPICUA_LIBC_INTERNAL_H
