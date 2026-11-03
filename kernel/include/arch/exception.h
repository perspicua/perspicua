/*
 * exception.h - Hardware exception handling and trap frame definitions.
 */

#ifndef PERSPICUA_ARCH_EXCEPTION_H
#define PERSPICUA_ARCH_EXCEPTION_H

#define EXCEPTION_STACK_SIZE      16384
#define EXCEPTION_STACK_SHIFT     14
#define EXCEPTION_TRAP_FRAME_SIZE 288
#define EXCEPTION_NESTED_HEADROOM 4096

#ifndef __ASSEMBLER__

    #include <stdint.h>

    #include "arch/cpu.h"
    #include "sched/sched.h"

/*
 * struct exception_trap_frame - CPU state saved on the stack during an exception.
 *
 * General-purpose registers only. Nothing here is FP or SIMD state: the
 * toolchain builds every target with -mgeneral-regs-only, boot.S leaves
 * CPACR_EL1.FPEN clear so an FP instruction traps rather than running against
 * unsaved registers, and saving q0-q31 cost 512 bytes of stores and loads on
 * every single trap for state no code ever produced or read.
 *
 * Restoring that is a lazy-FP implementation: trap the first use per task,
 * enable FPEN for it, and save its registers on switch. Adding the stores back
 * to this path unconditionally is not.
 *
 * The layout MUST strictly match the assembly macros in vector.S.
 */
struct exception_trap_frame {
    uint64_t sp_el0;
    uint64_t sp_el1; // the interrupted SP for EL1 frames, 0 for EL0 frames.
    uintptr_t elr_el1;
    uint64_t spsr_el1;

    uint64_t x[30];
    uint64_t x30;
    uint64_t _pad_end; // keeps the frame a multiple of 16 for sp alignment
} __attribute__((aligned(16)));

/*
 * save_all and restore_all in vector.S reach into this frame with hardcoded
 * offsets and reserve its size on the stack. A field reordered or inserted here
 * would silently corrupt every exception rather than fail to build.
 */
_Static_assert(sizeof(struct exception_trap_frame) == EXCEPTION_TRAP_FRAME_SIZE,
               "trap frame size — update vector.S");
_Static_assert(__builtin_offsetof(struct exception_trap_frame, sp_el0) == 0, "sp_el0 offset");
_Static_assert(__builtin_offsetof(struct exception_trap_frame, sp_el1) == 8, "sp_el1 offset");
_Static_assert(__builtin_offsetof(struct exception_trap_frame, elr_el1) == 16, "elr_el1 offset");
_Static_assert(__builtin_offsetof(struct exception_trap_frame, spsr_el1) == 24, "spsr_el1 offset");
_Static_assert(__builtin_offsetof(struct exception_trap_frame, x) == 32, "x[] offset");
_Static_assert(__builtin_offsetof(struct exception_trap_frame, x30) == 272, "x30 offset");

_Static_assert((EXCEPTION_STACK_SIZE & (EXCEPTION_STACK_SIZE - 1)) == 0,
               "EXCEPTION_STACK_SIZE must be a power of two");
_Static_assert((1ULL << EXCEPTION_STACK_SHIFT) == EXCEPTION_STACK_SIZE,
               "EXCEPTION_STACK_SHIFT mismatch");
_Static_assert(EXCEPTION_NESTED_HEADROOM < EXCEPTION_STACK_SIZE / 2,
               "a nested exception needs room for at least one more level");

extern uint8_t exception_stacks[CPU_MAX_CORES][EXCEPTION_STACK_SIZE];

void exception_unhandled_vector(void);

void exception_irq_handler(void);

void exception_sync_handler(struct exception_trap_frame *tf);
#endif // __ASSEMBLER__

#endif // PERSPICUA_ARCH_EXCEPTION_H
