ENTRY(_entry)

KERNEL_VMA = 0xFFFFFF8000000000;
PHYS_MEM   = 0x0000000000080000;

SECTIONS
{
    /* The virtual address where the kernel runs */
    . = KERNEL_VMA + PHYS_MEM;
    __kernel_start = .;

    /* AT() tells the linker to put it at the physical address in the binary */
    .text : AT(ADDR(.text) - KERNEL_VMA) ALIGN(4096) { 
        __text_start = .;
        KEEP(*(.text.boot))
        *(.text .text.*) 
        __text_end = .;
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_VMA) ALIGN(4096) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    }

    .data : AT(ADDR(.data) - KERNEL_VMA) ALIGN(4096) {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    }

    .bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_VMA) ALIGN(4096) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(4096);
    _stack_bottom = .;
    . += 0x40000;
    _stack_top = .;

    . = ALIGN(4096);
    __kernel_end = .;
}
