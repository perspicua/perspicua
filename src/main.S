.global main
main:
    stp x29, x30, [sp, #-16]!
    mov x29, sp

    mov x0, 'H'
    bl print_char
    mov x0, 'e'
    bl print_char
    mov x0, 'l'
    bl print_char
    mov x0, 'l'
    bl print_char
    mov x0, 'o'
    bl print_char

    mov x0, ' '
    bl print_char

    mov x0, 'W'
    bl print_char
    mov x0, 'o'
    bl print_char
    mov x0, 'r'
    bl print_char
    mov x0, 'l'
    bl print_char
    mov x0, 'd'
    bl print_char
    mov x0, '!'
    bl print_char
    mov x0, '\n'
    bl print_char

    ldp x29, x30, [sp], #16
    ret

# Prints a character from the x0 register
print_char:
    ldr x3, =0xFE201000
    str x0, [x3]
    ret
