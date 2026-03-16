.section .rodata
.align 12

.global initrd_start
.global initrd_end

initrd_start:
    .quad 0
initrd_end:
    .quad 0
