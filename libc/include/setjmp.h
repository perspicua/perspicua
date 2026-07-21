/*
 * setjmp.h - Non-local jumps.
 *
 * Minimal AArch64 variant: the integer callee-saved registers, the frame
 * pointer, the link register and the stack pointer are saved. The callee-saved
 * floating-point registers (d8-d15) are NOT saved, so a jump across code that
 * relies on their values being preserved is unsupported.
 */

#ifndef PERSPICUA_LIBC_SETJMP_H
#define PERSPICUA_LIBC_SETJMP_H

/*
 * Saved state: x19-x28, x29 (fp), x30 (lr), sp.
 *
 * The layout is fixed by libc/arch/setjmp.S, which addresses these slots by
 * byte offset — keep the two in step.
 */
#define _JMP_BUF_WORDS 13

typedef unsigned long jmp_buf[_JMP_BUF_WORDS] __attribute__((aligned(16)));

/*
 * setjmp - Records the current execution context and returns 0.
 *
 * Returns a second time, with a non-zero value, whenever longjmp is called on
 * the same buffer. Locals of the calling function that are modified between
 * the two returns must be declared volatile, otherwise their value after a
 * longjmp is unspecified.
 */
int setjmp(jmp_buf env) __attribute__((returns_twice));

/*
 * longjmp - Resumes execution at the matching setjmp, which returns val.
 *
 * A val of 0 is reported as 1, so the resumed setjmp can never be mistaken
 * for a first-time return. The frames entered since setjmp are abandoned.
 */
__attribute__((noreturn)) void longjmp(jmp_buf env, int val);

#endif /* PERSPICUA_LIBC_SETJMP_H */
