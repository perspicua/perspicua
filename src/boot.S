.section ".text.boot"

.global _entry
_entry:
    mrs x0, mpidr_el1
    and x0, x0, #3
    cbz x0, 2f

1:  wfe
    b 1b

2:
    mrs x0, CurrentEL
    lsr x0, x0, #2
    cmp x0, #2
    b.ne 3f

    // currently in el2, drop to el1
    ldr x0, =(1 << 31)
    msr hcr_el2, x0

    ldr x0, =0x3c5
    msr spsr_el2, x0

    adr x0, 3f
    msr elr_el2, x0

    eret

3:  ldr x0, =vector_table
    msr vbar_el1, x0

    ldr x1, =_stack_top
    mov sp, x1

    ldr x1, =__bss_start
    ldr x2, =__bss_end
    cmp x1, x2
    b.eq 4f
    str xzr, [x1], #8   // Write 8 bytes of zero (xzr) and increment x1
    b 3b

4:
    // jump to C
    bl main

_entry_loop:
    wfe
    b _entry_loop
