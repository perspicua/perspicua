SECTIONS
{
    . = 0x80000;
    .text : { 
        KEEP(*(.text.boot))
        *(.text .text.*) 
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss (NOLOAD) : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /* The stack must be aligned to 16 bits for aarch64 */
    . = ALIGN(16);
    _stack_bottom = .;
    /* Reserve 64K for the stack */
    . += 0x10000;
    _stack_top = .;
}
