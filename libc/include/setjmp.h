/*
 * setjmp.h - Non-local jumps.
 */

#ifndef PERSPICUA_LIBC_SETJMP_H
#define PERSPICUA_LIBC_SETJMP_H

// x19-x28, fp, lr, sp, at the byte offsets libc/arch/setjmp.S uses.
#define _JMP_BUF_WORDS 13

typedef unsigned long jmp_buf[_JMP_BUF_WORDS] __attribute__((aligned(16)));

// 0 now, then longjmp's value; a local changed between the two returns must be volatile.
int setjmp(jmp_buf env) __attribute__((returns_twice));

// Resumes at the matching setjmp, which returns val, or 1 for a val of 0.
__attribute__((noreturn)) void longjmp(jmp_buf env, int val);

#endif // PERSPICUA_LIBC_SETJMP_H
