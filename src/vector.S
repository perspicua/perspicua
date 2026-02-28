.section ".text"

.macro ventry label
    .align 7
    b \label
.endm

// vector table
// must be aligned to 2048 bytes (2^11)
.align 11
.global vector_table
vector_table:
    // Current EL using SP_EL0
    ventry default_exception_handler // Synchronous
    ventry default_exception_handler // IRQ
    ventry default_exception_handler // FIQ
    ventry default_exception_handler // SError

    // Current EL using SP_ELx
    ventry default_exception_handler // Synchronous
    ventry default_exception_handler // IRQ
    ventry default_exception_handler // FIQ
    ventry default_exception_handler // SError

    // lower EL running AArch64 (User Space)
    ventry default_exception_handler // Synchronous
    ventry default_exception_handler // IRQ
    ventry default_exception_handler // FIQ
    ventry default_exception_handler // SError

    // Lower EL running AArch32
    ventry default_exception_handler // Synchronous
    ventry default_exception_handler // IRQ
    ventry default_exception_handler // FIQ
    ventry default_exception_handler // SError

default_exception_handler:
    bl c_exception_handler
1:  wfe
    b 1b
