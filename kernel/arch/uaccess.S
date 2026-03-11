.section ".text"

// int copy_from_user(void *dest, const void *src, size_t n)
// x0 = dest, x1 = src, x2 = n
.global copy_from_user
copy_from_user:
    cbz x2, .Ldone_from
.Lloop_from:
.Lload_from:
    ldrb w3, [x1], #1
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
    strb w3, [x0], #1
    subs x2, x2, #1
    b.ne .Lloop_to
.Ldone_to:
    mov x0, #0
    ret

.Lfault_to:
    mov x0, x2
    ret

.section "__ex_table", "a"
.align 3
    .quad .Lload_from
    .quad .Lfault_from
    .quad .Lstore_to
    .quad .Lfault_to
