.section ".text"

// int copy_from_user(void *dest, const void *src, size_t n)
// x0 = dest, x1 = src, x2 = n
.global copy_from_user
copy_from_user:
    cbz x2, .Ldone
.Lloop:
.Lload:
    ldrb w3, [x1], #1
    strb w3, [x0], #1
    subs x2, x2, #1
    b.ne .Lloop
.Ldone:
    mov x0, #0
    ret

.Lfault:
    mov x0, x2  // Return the number of bytes not copied
    ret

.section "__ex_table", "a"
.align 3
    // .quad stores an 8-byte address. We store the faulting instruction address
    // followed by the fixup handler address.
    .quad .Lload
    .quad .Lfault
