/*
 * exception.h - Hardware exception handling and trap frame definitions.
 *
 * This header defines the register state saved during exceptions and the
 * entry points for architecture-specific exception handling on AArch64.
 */

#ifndef PERSPICUA_ARCH_EXCEPTION_H
#define PERSPICUA_ARCH_EXCEPTION_H

#include "types.h"

#include "sched/sched.h"

/* 128-bit type for NEON/FPU registers */
typedef __uint128_t uint128_t;

/*
 * struct exception_trap_frame - CPU state saved on the stack during an exception.
 *
 * This structure captures the complete execution context (GPRs, ELR, SPSR, and
 * SIMD/FP registers) to allow for transparent process preemption and fault
 * diagnosis. The layout MUST strictly match the assembly macros in vector.S.
 */
struct exception_trap_frame {
    uint64_t sp_el0;
    uint64_t _pad;     /* Alignment padding (xzr) */
    uintptr_t elr_el1; /* Address to return to after exception or syscall */
    uint64_t spsr_el1; /* Saved processor state */

    uint64_t x[30]; /* General purpose registers x0 - x29 */
    uint64_t x30;   /* Link register (LR) */
    uint32_t fpsr;  /* Floating-point status register */
    uint32_t fpcr;  /* Floating-point control register */

    uint128_t q[32]; /* SIMD/NEON/FPU registers v0 - v31 */
} __attribute__((aligned(16)));

/*
 * save_all and restore_all in vector.S reach into this frame with hardcoded
 * offsets and reserve its size on the stack. A field reordered or inserted here
 * would silently corrupt every exception rather than fail to build.
 */
_Static_assert(sizeof(struct exception_trap_frame) == 800, "trap frame size — update vector.S");
_Static_assert(__builtin_offsetof(struct exception_trap_frame, sp_el0) == 0, "sp_el0 offset");
_Static_assert(__builtin_offsetof(struct exception_trap_frame, elr_el1) == 16, "elr_el1 offset");
_Static_assert(__builtin_offsetof(struct exception_trap_frame, spsr_el1) == 24, "spsr_el1 offset");
_Static_assert(__builtin_offsetof(struct exception_trap_frame, x) == 32, "x[] offset");
_Static_assert(__builtin_offsetof(struct exception_trap_frame, x30) == 272, "x30 offset");
_Static_assert(__builtin_offsetof(struct exception_trap_frame, fpsr) == 280, "fpsr offset");
_Static_assert(__builtin_offsetof(struct exception_trap_frame, fpcr) == 284, "fpcr offset");
_Static_assert(__builtin_offsetof(struct exception_trap_frame, q) == 288, "q[] offset");

/*
 * exception_unhandled_vector - Default handler for unknown or reserved vectors.
 *
 * Used as a fallback when the processor takes an exception into a vector
 * with no specific implementation (e.g., FIQ or SError).
 */
void exception_unhandled_vector(void);

/*
 * exception_irq_handler - Top-level entry point for hardware interrupts.
 */
void exception_irq_handler(void);

/*
 * exception_sync_handler - Entry point for synchronous traps and system calls.
 */
void exception_sync_handler(struct exception_trap_frame *tf);

/*
 * Interrupt statistics tracked per CPU core.
 */
struct irq_stats {
    uint64_t timer_count;
    uint64_t uart_count;
};

extern struct irq_stats core_irq_stats[SCHED_NUM_CORES];

#endif /* PERSPICUA_ARCH_EXCEPTION_H */
