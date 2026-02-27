.section ".text.boot"

.global _entry
_entry:
    # Setup the stack
    ldr x1, =_stack_top
    mov sp, x1

    bl main

_entry_loop:
    wfe
    b _entry_loop
