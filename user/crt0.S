#include "uapi/syscalls.h"

.section ".text"
.global __sigrestorer
__sigrestorer:
    mov x8, #SYS_SIGRETURN
    svc #0

.section ".text.startup"
.global _start

_start:
    // x0 = argc, x1 = argv , x2 = envp(passed by kernel in trap frame)
    mov x19, x0
    mov x20, x1
    mov x21, x2

    adrp x0, __sigrestorer
    add x0, x0, :lo12:__sigrestorer
    mov x8, #SYS_SIGRESTORE
    svc #0

    // Restore argc/argv for main
    mov x0, x19
    mov x1, x20
    mov x2, x21

    bl main
    // If main returns, call exit(0)
    bl sys_exit
1:  b 1b
