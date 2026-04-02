ENTRY(_entry)

KERNEL_VMA = 0xFFFFFF8000000000;
PHYS_MEM   = 0x0000000000080000;

PHDRS
{
    text    PT_LOAD FLAGS(5);   /* R + X */
    rodata  PT_LOAD FLAGS(4);   /* R */
    data    PT_LOAD FLAGS(6);   /* R + W */
}

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
    } :text

    .rodata : AT(ADDR(.rodata) - KERNEL_VMA) ALIGN(4096) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        . = ALIGN(8);
        __drivers_core_start = .;
        KEEP(*(.drivers.core))
        __drivers_core_end = .;
        
        __drivers_bus_start = .;
        KEEP(*(.drivers.bus))
        __drivers_bus_end = .;
        
        __drivers_device_start = .;
        KEEP(*(.drivers.device))
        __drivers_device_end = .;
        
        . = ALIGN(8);
        __ex_table_start = .;
        *(__ex_table)
        __ex_table_end = .;
        __rodata_end = .;
    } :rodata

    .data : AT(ADDR(.data) - KERNEL_VMA) ALIGN(4096) {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    } :data

    .bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_VMA) ALIGN(4096) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } :data

    . = ALIGN(4096);
    _stack_bottom = .;
    . += 0x40000;
    _stack_top = .;

    . = ALIGN(4096);
    __kernel_end = .;
}
