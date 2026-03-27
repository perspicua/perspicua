/*
 * kernel/include/arch/uaccess.h
 *
 * Safe access to user-space memory and exception fixup mechanisms.
 */

#ifndef PERSPICUA_ARCH_UACCESS_H
    #define PERSPICUA_ARCH_UACCESS_H

    #include "types.h"

/* Forward declaration for the exception_trap_frame struct */
struct exception_trap_frame;

/* --- Function Prototypes --- */

int validate_user_buffer(const void* ptr, size_t len, int writable);

/*
 * Safely copies data from user-space to kernel-space.
 * Returns the number of bytes that could NOT be copied (0 on success).
 */
int copy_from_user(void* dest, const void* src, size_t n);

/*
 * Safely copies data from kernel-space to user-space.
 * Returns the number of bytes that could NOT be copied (0 on success).
 */
int copy_to_user(void* dest, const void* src, size_t n);

/*
 * Checks if a fault occurred in a safe copy routine and applies fixups.
 * Returns 1 if a fixup was applied, 0 otherwise.
 */
int exception_fixup(struct exception_trap_frame* tf);

#endif /* PERSPICUA_ARCH_UACCESS_H */
long strncpy_from_user(char* dest, const char* src, long count);
