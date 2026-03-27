/*
 * panic.c - Implementation of the kernel panic handler.
 *
 * Halts all CPU cores on an unrecoverable error and emits a diagnostic
 * report containing:
 *   - Panic message and source location
 *   - System uptime at time of panic
 *   - Current task ID, PID, stack, and TTBR0
 *   - Full general-purpose register dump (from trap frame when available,
 *     otherwise a live EL1 snapshot)
 *   - Key system registers: ESR_EL1, FAR_EL1, SPSR_EL1, ELR_EL1
 *   - Stack backtrace via the frame pointer chain
 *
 * When panic is triggered from an exception handler via PANIC_TF(), the
 * trap frame is used for the register dump. This gives the actual faulting
 * register state rather than the EL1 snapshot inside panic_full() itself,
 * which would only reflect the panic call-site context.
 */
#include "panic.h"

#include "stdio.h"
#include "core/timer.h"
#include "driver/gic.h"
#include "sched/sched.h"
#include "arch/exception.h"

/* Global state for core synchronization during panic */
volatile int kernel_panicked = 0;

/* Maximum number of stack frames to walk during a backtrace */
#define PANIC_MAX_FRAMES 16

/* EC decode table shared by both register dump paths */
static void print_ec(unsigned int ec)
{
    switch (ec) {
        case 0x01:
            printf("  [WFI/WFE]\n");
            break;
        case 0x15:
            printf("  [SVC AArch64]\n");
            break;
        case 0x20:
            printf("  [Inst Abort, lower EL]\n");
            break;
        case 0x21:
            printf("  [Inst Abort, same EL]\n");
            break;
        case 0x24:
            printf("  [Data Abort, lower EL]\n");
            break;
        case 0x25:
            printf("  [Data Abort, same EL]\n");
            break;
        case 0x2C:
            printf("  [SP Alignment Fault]\n");
            break;
        case 0x30:
            printf("  [FP Exception]\n");
            break;
        case 0x3C:
            printf("  [BRK instruction]\n");
            break;
        default:
            printf("  [EC=0x%02x]\n", ec);
            break;
    }
}

/*
 * panic_dump_tf_registers - Prints the full register state from a trap frame.
 *
 * Called when panic originates from an exception handler. The trap frame
 * was saved by the low-level exception entry assembly before any C code ran,
 * so it accurately reflects the CPU state at the moment of the fault.
 */
static void panic_dump_tf_registers(struct exception_trap_frame *tf)
{
    printf("\n--- Registers (from exception trap frame) ---\n");

    /* x0–x28 in two columns */
    for (int i = 0; i < 28; i += 2) {
        printf("  x%-2d: 0x%016lx   x%-2d: 0x%016lx\n", i, tf->x[i], i + 1, tf->x[i + 1]);
    }
    /* x29 (fp) alone on last GPR line, then x30 (lr) */
    printf("  x29: 0x%016lx   x30: 0x%016lx\n", tf->x[29], tf->x30);

    printf("\n--- System Registers (from trap frame) ---\n");
    printf("  ELR_EL1  : 0x%016lx  (faulting PC)\n", tf->elr_el1);
    printf("  SP_EL0   : 0x%016lx\n", tf->sp_el0);
    printf("  SPSR_EL1 : 0x%016lx\n", tf->spsr_el1);

    /*
     * ESR and FAR are not saved in the trap frame but remain stable
     * across a synchronous exception until ERET, so reading them live
     * here is safe and accurate.
     */
    unsigned long esr, far_reg;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    asm volatile("mrs %0, far_el1" : "=r"(far_reg));

    printf("  ESR_EL1  : 0x%016lx", esr);
    print_ec((unsigned int)((esr >> 26) & 0x3F));
    printf("  FAR_EL1  : 0x%016lx\n", far_reg);
}

/*
 * panic_dump_live_registers - Captures and prints key AArch64 registers
 * from the current EL1 execution context.
 *
 * Used when panic is NOT triggered from an exception handler (e.g., a
 * failed ASSERT or an explicit PANIC() call). These values reflect the
 * state inside panic_full() itself, not the original fault site — for
 * that, use PANIC_TF() with the trap frame.
 */
static void panic_dump_live_registers(void)
{
    unsigned long sp, lr, spsr, esr, far_reg;

    asm volatile("mov %0, sp" : "=r"(sp));
    asm volatile("mov %0, x30" : "=r"(lr));
    asm volatile("mrs %0, spsr_el1" : "=r"(spsr));
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    asm volatile("mrs %0, far_el1" : "=r"(far_reg));

    printf("\n--- Registers (live EL1 snapshot) ---\n");
    printf("  SP       : 0x%016lx\n", sp);
    printf("  LR (x30) : 0x%016lx\n", lr);
    printf("  SPSR_EL1 : 0x%016lx\n", spsr);
    printf("  ESR_EL1  : 0x%016lx", esr);
    print_ec((unsigned int)((esr >> 26) & 0x3F));
    printf("  FAR_EL1  : 0x%016lx\n", far_reg);
}

/*
 * panic_backtrace - Walks the AArch64 frame pointer chain.
 *
 * AArch64 calling convention stores {FP, LR} at the base of each frame:
 *   [fp + 0] = previous frame pointer (x29)
 *   [fp + 8] = saved return address   (x30)
 *
 * When called with a trap frame, pass tf->x[29] so the trace starts at
 * the faulting code rather than inside panic_full().
 *
 * Requires -fno-omit-frame-pointer in CFLAGS.
 */
static void panic_backtrace(unsigned long fp)
{
    printf("\n--- Stack Trace ---\n");

    if (!fp) {
        printf("  (frame pointer is NULL — compiled without -fno-omit-frame-pointer?)\n");
        return;
    }

    for (int i = 0; i < PANIC_MAX_FRAMES; i++) {
        if (fp & 0x7UL) {
            printf("  #%-2d  [unaligned FP 0x%016lx — stopping]\n", i, fp);
            break;
        }

        unsigned long *frame = (unsigned long *)fp;
        unsigned long prev_fp = frame[0];
        unsigned long ret_addr = frame[1];

        printf("  #%-2d  0x%016lx\n", i, ret_addr);

        if (!prev_fp) {
            printf("  (end of stack)\n");
            break;
        }

        if (prev_fp <= fp) {
            printf("  #%-2d  [FP not advancing (0x%016lx) — stopping]\n", i + 1, prev_fp);
            break;
        }

        fp = prev_fp;
    }
}

/*
 * panic_dump_task - Prints the scheduler's view of the currently running task.
 */
static void panic_dump_task(void)
{
    printf("\n--- Current Task ---\n");

    struct task *t = sched_get_current();
    if (!t) {
        printf("  (no current task — scheduler not yet initialized)\n");
        return;
    }

    printf("  Task ID  : %lu\n", t->id);
    printf("  PID      : %u\n", t->pid);
    printf("  State    : %d\n", (int)t->state);
    printf("  Stack    : 0x%016lx\n", (unsigned long)t->stack);
    printf("  TTBR0    : 0x%016lx\n", t->ttbr0);
}

/*
 * panic_full - Primary kernel error handler.
 *
 * @msg:  Human-readable description of the fault.
 * @file: Source file (__FILE__).
 * @line: Line number (__LINE__).
 * @fp:   Frame pointer at the PANIC/ASSERT call site.
 * @tf:   Exception trap frame, or NULL if not called from an exception handler.
 *
 * When tf is non-NULL (PANIC_TF):
 *   - Dumps all 31 GPRs, ELR, SP_EL0, SPSR from the trap frame.
 *   - Starts the backtrace from tf->x[29] (the faulting FP).
 *
 * When tf is NULL (PANIC / ASSERT):
 *   - Dumps the live EL1 register snapshot (SP, LR, SPSR, ESR, FAR).
 *   - Starts the backtrace from the supplied fp argument.
 */
void panic_full(const char *msg, const char *file, int line, unsigned long fp,
                struct exception_trap_frame *tf)
{
    disable_interrupts();

    /*
     * Re-entrant panic guard: if printf or a diagnostic helper faults,
     * spin silently rather than recursing. The first panic's output is
     * already on the wire at that point.
     */
    if (kernel_panicked) {
        while (1)
            asm volatile("wfe");
    }

    kernel_panicked = 1;
    asm volatile("dsb ish" ::: "memory");

    gic_send_panic_ipi();

    unsigned long uptime_ms = get_system_time();

    printf("\n");
    printf("           *** KERNEL PANIC ***           \n");
    printf("\n");
    printf("  Message  : %s\n", msg);
    printf("  Location : %s:%d\n", file, line);
    printf("  Uptime   : %lu ms\n", uptime_ms);

    panic_dump_task();

    if (tf) {
        /*
         * Exception-originated panic: use the trap frame for a complete
         * and accurate picture of the faulting context.
         */
        panic_dump_tf_registers(tf);
        panic_backtrace(tf->x[29]);
    } else {
        /*
         * Software-originated panic (ASSERT, explicit PANIC call): use
         * the live EL1 snapshot and the caller-supplied frame pointer.
         */
        panic_dump_live_registers();
        panic_backtrace(fp);
    }

    printf("\n--- All CPU cores halted ---\n\n");

    while (1)
        asm volatile("wfe");
}
