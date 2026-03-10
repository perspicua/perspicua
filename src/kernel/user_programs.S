.section .rodata
.align 12

.global user_hello_start
.global user_hello_end
.global user_hello_elf_start
.global user_hello_elf_end

user_hello_start:
    .incbin "build/user/hello.bin"
user_hello_end:

user_hello_elf_start:
    .incbin "build/user/hello.elf"
user_hello_elf_end:
