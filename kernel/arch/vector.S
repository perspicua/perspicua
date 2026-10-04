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
    sub sp, sp, #288
    
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
    
    // Save x30 (offset 272); xzr clears _pad_end, which signal delivery copies
    // to the user stack along with the rest of the frame.
    stp x30, xzr, [sp, #272]

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

    add sp, sp, #288
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
