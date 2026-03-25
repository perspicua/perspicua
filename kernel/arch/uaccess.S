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
.global strncpy_from_user
strncpy_from_user:
    mov x4, x0          // Save original dest pointer
    cbz x2, .Ldone_str
.Lloop_str:
.Lload_str:
    ldtrb w3, [x1]
    add x1, x1, #1
    strb w3, [x0], #1
    cbz w3, .Ldone_str
    subs x2, x2, #1
    b.ne .Lloop_str
.Ldone_str:
    sub x0, x0, x4       // Calculate copied bytes (including null if written)
    ret

.Lfault_str:
    mov x0, #-14         // -EFAULT
    ret

.section "__ex_table", "a"
.align 3
    .quad .Lload_from
    .quad .Lfault_from
    .quad .Lloop_to
    .quad .Lfault_to
    .quad .Lstore_to
    .quad .Lfault_to
    .quad .Lload_str
    .quad .Lfault_str
