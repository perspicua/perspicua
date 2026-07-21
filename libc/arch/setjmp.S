/*
 * setjmp.S - AArch64 implementation of setjmp and longjmp.
 *
 * Saves the AAPCS64 integer callee-saved set (x19-x28), the frame pointer,
 * the link register and the stack pointer. x0-x18 are caller-saved, so the
 * compiler already treats them as clobbered across the call to setjmp and
 * they need no preserving. The callee-saved FP registers d8-d15 are not
 * saved; see libc/include/setjmp.h.
 *
 * Slot offsets must match _JMP_BUF_WORDS and the layout comment in setjmp.h.
 */

.section ".text"

/*
 * int setjmp(jmp_buf env)
 *   x0 = env
 *
 * Returns 0 here; returns again from longjmp with that call's value.
 */
.global setjmp
.type setjmp, %function
setjmp:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]   // fp, lr
    mov x9, sp
    str x9, [x0, #96]         // sp

    mov w0, #0
    ret
.size setjmp, . - setjmp

/*
 * void longjmp(jmp_buf env, int val)
 *   x0 = env, w1 = val
 *
 * Restoring x30 makes the ret below land at the instruction after the
 * original `bl setjmp`, with the return value in w0. Restoring sp abandons
 * every frame entered since setjmp, which is the intended behaviour.
 */
.global longjmp
.type longjmp, %function
longjmp:
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]   // fp, lr
    ldr x9, [x0, #96]
    mov sp, x9                // sp

    // val of 0 must surface at the setjmp site as 1, never as a first return.
    cmp w1, #0
    csinc w0, w1, wzr, ne

    ret
.size longjmp, . - longjmp
