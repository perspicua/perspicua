SECTIONS
{
    . = 0x80000;
    .text : { 
        KEEP(*(.text.boot))
        *(.text .text.*) 
    }

    /* The stack must be aligned to 16 bits for aarch64 */
    . = ALIGN(16);
    _stack_bottom = .;
    /* Reserve 64K for the stack */
    . += 0x10000;
    _stack_top = .;
}
