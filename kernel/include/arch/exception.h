/*
 * kernel/include/arch/exception.h
 *
 * Hardware exception handling, trap frames, and vector handlers for AArch64.
 */

#ifndef PERSPICUA_ARCH_EXCEPTION_H
#define PERSPICUA_ARCH_EXCEPTION_H

#include "types.h"

/* --- Constants and Macros --- */

/* 128-bit type for NEON/FPU registers */
typedef __uint128_t uint128_t;

/* --- Data Structures --- */

/*
 * Represents the CPU state saved on the stack during an exception.
 * Layout must strictly match the save_all/restore_all macros in arch/vector.S.
 */
struct exception_trap_frame
{
    uint64_t sp_el0;
    uint64_t _pad;     // alignment padding (xzr)
    uintptr_t elr_el1; // addr to return after syscall
    uint64_t spsr_el1; // cpu state
    // registers
    uint64_t x[30]; // x0 - x29
    uint64_t x30;   // link
    uint32_t fpsr;
    uint32_t fpcr;

    uint128_t q[32]; // NEON/FPU registers
} __attribute__((aligned(16)));

/* --- Function Prototypes --- */

/*
 * Called when an unexpected exception occurs that has no specific handler.
 */
void exception_unhandled_vector(void);

/*
 * Main entry point for hardware interrupts (IRQ).
 */
void exception_irq_handler(void);

/*
 * Main entry point for synchronous exceptions (syscalls, aborts).
 */
void exception_sync_handler(struct exception_trap_frame* tf);

#endif /* PERSPICUA_ARCH_EXCEPTION_H */
