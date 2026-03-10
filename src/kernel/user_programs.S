.section .rodata
.align 12

.global initrd_start
.global initrd_end

initrd_start:
    .incbin "build/initrd.cpio"
initrd_end:
