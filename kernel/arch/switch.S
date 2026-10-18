/*
 * switch.S - Context switch between kernel tasks, and the first-run trampoline.
 */

.section ".text"

// switch_context(prev = x0, next = x1): saves the callee-saved state in prev, then loads next's.
.global switch_context
switch_context:
    // save callee-saved registers into prev context
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]   // fp, lr
    mov x9, sp
    str x9, [x0, #96]         // sp

    // restore callee-saved registers from next context
    ldp x19, x20, [x1, #0]
    ldp x21, x22, [x1, #16]
    ldp x23, x24, [x1, #32]
    ldp x25, x26, [x1, #48]
    ldp x27, x28, [x1, #64]
    ldp x29, x30, [x1, #80]   // fp, lr
    ldr x9, [x1, #96]
    mov sp, x9                 // sp

    dsb ish
    ret

.global task_wrapper_asm
task_wrapper_asm:
    bl  sched_task_start
    bl  enable_interrupts
    blr x19             // the entry point, kept across the calls as x19 is callee-saved
    bl  disable_interrupts
    bl  sched_exit_current
1:  wfe
    b 1b
