.section ".text.boot"

.global _entry
_entry:

    ldr x1, =_stack_top
    mov sp, x1

    ldr x1, =__bss_start
    ldr x2, =__bss_end
1:  cmp x1, x2
    b.eq 2f
    str xzr, [x1], #8   // Write 8 bytes of zero (xzr) and increment x1
    b 1b
    
2:
    // jump to C
    bl main

_entry_loop:
    wfe
    b _entry_loop
