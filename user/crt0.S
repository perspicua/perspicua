.section ".text.startup"
.global _start

_start:
    // User stack is allocated by the kernel and passed in SP_EL0
    // No initialization needed yet, just jump to main
    bl main
    // If main returns, call exit(0)
    bl sys_exit
1:  b 1b
