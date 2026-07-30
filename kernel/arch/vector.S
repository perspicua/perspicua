.section ".text"

.macro ventry label
    .balign 0x80
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
    ventry sync_handler // Synchronous
    ventry irq_handler               // IRQ
    ventry fiq_discard               // FIQ
    ventry default_exception_handler // SError

    // lower EL running AArch64 (User Space)
    ventry sync_handler // Synchronous
    ventry irq_handler               // IRQ
    ventry fiq_discard               // FIQ
    ventry default_exception_handler // SError

    // Lower EL running AArch32
    ventry sync_handler // Synchronous
    ventry default_exception_handler // IRQ
    ventry default_exception_handler // FIQ
    ventry default_exception_handler // SError

default_exception_handler:
    bl exception_unhandled_vector
1:  wfe
    b 1b

fiq_discard:
    stp x0, x1, [sp, #-16]!
    mrs x0, spsr_el1
    orr x0, x0, #(1 << 6)      // set PSTATE.F in SPSR -> mask FIQ
    msr spsr_el1, x0
    ldp x0, x1, [sp], #16
    eret

.macro save_all
    sub sp, sp, #800
    
    // Save x0-x29 (offset 32)
    stp x0, x1, [sp, #32 + 16 * 0]
    stp x2, x3, [sp, #32 + 16 * 1]
    stp x4, x5, [sp, #32 + 16 * 2]
    stp x6, x7, [sp, #32 + 16 * 3]
    stp x8, x9, [sp, #32 + 16 * 4]
    stp x10, x11, [sp, #32 + 16 * 5]
    stp x12, x13, [sp, #32 + 16 * 6]
    stp x14, x15, [sp, #32 + 16 * 7]
    stp x16, x17, [sp, #32 + 16 * 8]
    stp x18, x19, [sp, #32 + 16 * 9]
    stp x20, x21, [sp, #32 + 16 * 10]
    stp x22, x23, [sp, #32 + 16 * 11]
    stp x24, x25, [sp, #32 + 16 * 12]
    stp x26, x27, [sp, #32 + 16 * 13]
    stp x28, x29, [sp, #32 + 16 * 14]
    
    // Save x30 (offset 272)
    str x30, [sp, #272]

    // Save fpsr, fpcr (offset 280)
    mrs x0, fpsr
    str w0, [sp, #280]
    mrs x0, fpcr
    str w0, [sp, #284]

    // Save q0-q31 (offset 288)
    stp q0, q1, [sp, #288 + 32 * 0]
    stp q2, q3, [sp, #288 + 32 * 1]
    stp q4, q5, [sp, #288 + 32 * 2]
    stp q6, q7, [sp, #288 + 32 * 3]
    stp q8, q9, [sp, #288 + 32 * 4]
    stp q10, q11, [sp, #288 + 32 * 5]
    stp q12, q13, [sp, #288 + 32 * 6]
    stp q14, q15, [sp, #288 + 32 * 7]
    stp q16, q17, [sp, #288 + 32 * 8]
    stp q18, q19, [sp, #288 + 32 * 9]
    stp q20, q21, [sp, #288 + 32 * 10]
    stp q22, q23, [sp, #288 + 32 * 11]
    stp q24, q25, [sp, #288 + 32 * 12]
    stp q26, q27, [sp, #288 + 32 * 13]
    stp q28, q29, [sp, #288 + 32 * 14]
    stp q30, q31, [sp, #288 + 32 * 15]

    // Save ELR_EL1 and SPSR_EL1 (offset 16)
    mrs x0, elr_el1
    mrs x1, spsr_el1
    stp x0, x1, [sp, #16]

    // Save sp_el0 (offset 0)
    mrs x0, sp_el0
    stp x0, xzr, [sp, #0]
.endm

.macro restore_all
    // Restore sp_el0
    ldp x0, x1, [sp, #0]
    msr sp_el0, x0

    // Restore ELR_EL1 and SPSR_EL1
    ldp x0, x1, [sp, #16]
    msr elr_el1, x0
    msr spsr_el1, x1

    // Restore q0-q31
    ldp q0, q1, [sp, #288 + 32 * 0]
    ldp q2, q3, [sp, #288 + 32 * 1]
    ldp q4, q5, [sp, #288 + 32 * 2]
    ldp q6, q7, [sp, #288 + 32 * 3]
    ldp q8, q9, [sp, #288 + 32 * 4]
    ldp q10, q11, [sp, #288 + 32 * 5]
    ldp q12, q13, [sp, #288 + 32 * 6]
    ldp q14, q15, [sp, #288 + 32 * 7]
    ldp q16, q17, [sp, #288 + 32 * 8]
    ldp q18, q19, [sp, #288 + 32 * 9]
    ldp q20, q21, [sp, #288 + 32 * 10]
    ldp q22, q23, [sp, #288 + 32 * 11]
    ldp q24, q25, [sp, #288 + 32 * 12]
    ldp q26, q27, [sp, #288 + 32 * 13]
    ldp q28, q29, [sp, #288 + 32 * 14]
    ldp q30, q31, [sp, #288 + 32 * 15]

    // Restore fpsr, fpcr
    ldr w0, [sp, #280]
    msr fpsr, x0
    ldr w0, [sp, #284]
    msr fpcr, x0

    // Restore x30
    ldr x30, [sp, #272]

    // Restore x0-x29
    ldp x28, x29, [sp, #32 + 16 * 14]
    ldp x26, x27, [sp, #32 + 16 * 13]
    ldp x24, x25, [sp, #32 + 16 * 12]
    ldp x22, x23, [sp, #32 + 16 * 11]
    ldp x20, x21, [sp, #32 + 16 * 10]
    ldp x18, x19, [sp, #32 + 16 * 9]
    ldp x16, x17, [sp, #32 + 16 * 8]
    ldp x14, x15, [sp, #32 + 16 * 7]
    ldp x12, x13, [sp, #32 + 16 * 6]
    ldp x10, x11, [sp, #32 + 16 * 5]
    ldp x8, x9, [sp, #32 + 16 * 4]
    ldp x6, x7, [sp, #32 + 16 * 3]
    ldp x4, x5, [sp, #32 + 16 * 2]
    ldp x2, x3, [sp, #32 + 16 * 1]
    ldp x0, x1, [sp, #32 + 16 * 0]

    add sp, sp, #800
    eret
.endm

.global sync_handler
sync_handler:
    save_all

    // Interrupts stay masked here. ESR_EL1 and FAR_EL1 are not banked, so they
    // must be read before anything else can be taken on this core; the handler
    // captures them and unmasks itself. See exception_sync_handler.
    mov x0, sp
    bl exception_sync_handler
    b ret_to_user

.global irq_handler
irq_handler:
    save_all
    bl exception_irq_handler
    // fallthrough to ret_to_user

.global ret_to_user
ret_to_user:
    msr daifset, #2            // the return path itself must not be interrupted

    ldr x0, [sp, #24]          // tf->spsr_el1 (saved), not the live register
    and x0, x0, #0xf
    cbnz x0, 1f

    mov x0, sp
    bl signal_handle_pending
1:
    restore_all
