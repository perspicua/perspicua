/*
 * uaccess.h - Fault-safe copies between kernel and user address spaces.
 */

#ifndef PERSPICUA_ARCH_UACCESS_H
    #define PERSPICUA_ARCH_UACCESS_H

    #include <stddef.h>

// Forward declaration for the exception_trap_frame struct
struct exception_trap_frame;

int syscall_validate_user_buffer(const void *ptr, size_t len, int writable);

// Returns the bytes not copied; 0 on success.
int copy_from_user(void *dest, const void *src, size_t n);

// Returns the bytes not copied; 0 on success.
int copy_to_user(void *dest, const void *src, size_t n);

/*
 * Checks if a fault occurred in a safe copy routine and applies fixups.
 * Returns 1 if a fixup was applied, 0 otherwise.
 */
int exception_fixup(struct exception_trap_frame *tf);

#endif // PERSPICUA_ARCH_UACCESS_H
// At most count - 1 bytes, always terminated; the length, -ENAMETOOLONG, or -EINVAL on a fault.
long strncpy_from_user(char *dest, const char *src, long count);
