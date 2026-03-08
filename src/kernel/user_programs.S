.section .rodata
.align 12

.global user_hello_start
.global user_hello_end

user_hello_start:
    .incbin "build/user/hello.bin"
user_hello_end:
