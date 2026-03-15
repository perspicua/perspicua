.section ".text"

// void switch_context(struct cpu_context *prev, struct cpu_context *next)
// x0 = prev, x1 = next
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

    // for new tasks, x19 holds the entry point arg
    mov x0, x19

    dsb ish
    ret

.global task_wrapper_asm
task_wrapper_asm:
    mov x0, x19         // x19 holds the entry point
    bl  enable_interrupts
    blr x0              // call entry point
    bl  disable_interrupts
    bl  sched_get_current
    cbz x0, 1f
    mov x1, #3          // SCHED_TASK_DEAD
    str x1, [x0, #112]  // offset of 'state' in struct task
    bl  schedule
1:  wfe
    b 1b
