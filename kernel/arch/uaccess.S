#include "uapi/errors.h"

.section ".text"

// int copy_from_user(void *dest, const void *src, size_t n)
// x0 = dest, x1 = src, x2 = n
.global copy_from_user
copy_from_user:
    cbz x2, .Ldone_from
.Lloop_from:
.Lload_from:
    ldtrb w3, [x1]
    add x1, x1, #1
    strb w3, [x0], #1
    subs x2, x2, #1
    b.ne .Lloop_from
.Ldone_from:
    mov x0, #0
    ret

.Lfault_from:
    mov x0, x2  // Return the number of bytes not copied
    ret

// int copy_to_user(void *dest, const void *src, size_t n)
// x0 = dest, x1 = src, x2 = n
.global copy_to_user
copy_to_user:
    cbz x2, .Ldone_to
.Lloop_to:
    ldrb w3, [x1], #1
.Lstore_to:
    sttrb w3, [x0]
    add x0, x0, #1
    subs x2, x2, #1
    b.ne .Lloop_to
.Ldone_to:
    mov x0, #0
    ret

.Lfault_to:
    mov x0, x2
    ret

// long strncpy_from_user(char *dest, const char *src, long count)
// x0 = dest, x1 = src, x2 = count
//
// Copies at most count-1 bytes and always terminates dest when count >= 1, so
// a caller can never be handed an unterminated string to strlen(). Returns the
// length excluding the terminator, or a negative error on fault or truncation.
.global strncpy_from_user
strncpy_from_user:
    cmp x2, #1
    b.lt .Lstr_toolong  // No room even for a terminator
    mov x4, x0          // Start of dest, for the length and the fault path
    mov x5, x2          // Bytes of dest still available
.Lloop_str:
.Lload_str:
    ldtrb w3, [x1]
    add x1, x1, #1
    strb w3, [x0], #1
    cbz w3, .Lstr_done
    subs x5, x5, #1
    b.ne .Lloop_str

    // dest is full and the source had not ended: terminate over the last byte
    sub x0, x0, #1
    strb wzr, [x0]
    b .Lstr_toolong

.Lstr_done:
    sub x0, x0, x4
    sub x0, x0, #1      // Exclude the terminator
    ret

.Lfault_str:
    strb wzr, [x4]      // Leave an empty string rather than a partial one
    mov x0, #-PERS_ERR_INVALID_ARGUMENT
    ret

.Lstr_toolong:
    mov x0, #-PERS_ERR_NAME_TOO_LONG
    ret

.section "__ex_table", "a"
.align 3
    .quad .Lload_from
    .quad .Lfault_from
    .quad .Lstore_to
    .quad .Lfault_to
    .quad .Lload_str
    .quad .Lfault_str
