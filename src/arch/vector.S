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
    ventry irq_handler               // IRQ
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

.global irq_handler
irq_handler:
    // save ELR_EL1 and SPSR_EL1 so context switches preserve them
    mrs x0, elr_el1
    mrs x1, spsr_el1
    stp x0, x1, [sp, #-16]!

    // save GPRs (x0-x30)
    sub sp, sp, #256
    stp x0, x1, [sp, #16 * 0]
    stp x2, x3, [sp, #16 * 1]
    stp x4, x5, [sp, #16 * 2]
    stp x6, x7, [sp, #16 * 3]
    stp x8, x9, [sp, #16 * 4]
    stp x10, x11, [sp, #16 * 5]
    stp x12, x13, [sp, #16 * 6]
    stp x14, x15, [sp, #16 * 7]
    stp x16, x17, [sp, #16 * 8]
    stp x18, x19, [sp, #16 * 9]
    stp x20, x21, [sp, #16 * 10]
    stp x22, x23, [sp, #16 * 11]
    stp x24, x25, [sp, #16 * 12]
    stp x26, x27, [sp, #16 * 13]
    stp x28, x29, [sp, #16 * 14]
    str x30, [sp, #16 * 15]

    bl c_irq_handler

    ldr x30, [sp, #16 * 15]
    ldp x28, x29, [sp, #16 * 14]
    ldp x26, x27, [sp, #16 * 13]
    ldp x24, x25, [sp, #16 * 12]
    ldp x22, x23, [sp, #16 * 11]
    ldp x20, x21, [sp, #16 * 10]
    ldp x18, x19, [sp, #16 * 9]
    ldp x16, x17, [sp, #16 * 8]
    ldp x14, x15, [sp, #16 * 7]
    ldp x12, x13, [sp, #16 * 6]
    ldp x10, x11, [sp, #16 * 5]
    ldp x8, x9, [sp, #16 * 4]
    ldp x6, x7, [sp, #16 * 3]
    ldp x4, x5, [sp, #16 * 2]
    ldp x2, x3, [sp, #16 * 1]
    ldp x0, x1, [sp, #16 * 0]
    add sp, sp, #256

    // restore ELR_EL1 and SPSR_EL1
    ldp x21, x22, [sp], #16
    msr elr_el1, x21
    msr spsr_el1, x22

    eret
