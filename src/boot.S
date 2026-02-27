.section ".text.boot"

.global _entry
_entry:
    mrs x0, mpidr_el1
    and x0, x0, #3
    cbz x0, 2f

1:  wfe
    b 1b

2:
    ldr x1, =_stack_top
    mov sp, x1

    ldr x1, =__bss_start
    ldr x2, =__bss_end
3:  cmp x1, x2
    b.eq 4f
    str xzr, [x1], #8   // Write 8 bytes of zero (xzr) and increment x1
    b 3b

4:
    // jump to C
    bl main

_entry_loop:
    wfe
    b _entry_loop
