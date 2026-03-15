#include "uapi/syscalls.h"

.section ".text"
.global __sigrestorer
__sigrestorer:
    mov x8, #SYS_SIGRETURN
    svc #0

.section ".text.startup"
.global _start

_start:
    adrp x0, __sigrestorer
    add x0, x0, :lo12:__sigrestorer
    mov x8, #SYS_SIGRESTORE
    svc #0

    // User stack is allocated by the kernel and passed in SP_EL0
    // No initialization needed yet, just jump to main
    bl main
    // If main returns, call exit(0)
    bl sys_exit
1:  b 1b
