ENTRY(_start)

PHDRS
{
    text    PT_LOAD FLAGS(5);   /* R + X */
    data    PT_LOAD FLAGS(6);   /* R + W */
}

SECTIONS
{
    . = 0x100000;

    .text : ALIGN(4096) {
        *(.text.startup)
        *(.text*)
    } :text

    .rodata : ALIGN(4096) {
        *(.rodata*)
    } :text

    .data : ALIGN(4096) {
        *(.data*)
    } :data

    .bss : ALIGN(4096) {
        *(.bss*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.note*)
    }
}
