/*
 * panic.c - Implementation of the kernel panic handler.
 *
 * Halts all CPU cores on an unrecoverable error and emits a diagnostic
 * report containing register state, task info, and stack backtrace.
 */

#include "panic.h"

#include "stdio.h"

#include "arch/exception.h"

#include "core/timer.h"
#include "driver/gic.h"
#include "sched/sched.h"

/* Global state for core synchronization during panic. */
volatile int kernel_panicked = 0;

#define PANIC_MAX_FRAMES 16

/* Decodes Exception Class (EC) for diagnostic output. */
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

/* Prints the full register state from a trap frame. */
static void panic_dump_tf_registers(struct exception_trap_frame *tf)
{
    printf("\n--- Registers (from exception trap frame) ---\n");

    for (int i = 0; i < 28; i += 2) {
        printf("  x%-2d: 0x%016lx   x%-2d: 0x%016lx\n", i, tf->x[i], i + 1, tf->x[i + 1]);
    }
    printf("  x29: 0x%016lx   x30: 0x%016lx\n", tf->x[29], tf->x30);

    printf("\n--- System Registers (from trap frame) ---\n");
    printf("  ELR_EL1  : 0x%016lx  (faulting PC)\n", tf->elr_el1);
    printf("  SP_EL0   : 0x%016lx\n", tf->sp_el0);
    printf("  SPSR_EL1 : 0x%016lx\n", tf->spsr_el1);

    unsigned long esr, far_reg;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    asm volatile("mrs %0, far_el1" : "=r"(far_reg));

    printf("  ESR_EL1  : 0x%016lx", esr);
    print_ec((unsigned int)((esr >> 26) & 0x3F));
    printf("  FAR_EL1  : 0x%016lx\n", far_reg);
}

/* Captures and prints key AArch64 registers from EL1 context. */
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

/* Walks the AArch64 frame pointer chain. */
static void panic_backtrace(unsigned long fp)
{
    printf("\n--- Stack Trace ---\n");

    if (!fp) {
        printf("  (frame pointer is NULL)\n");
        return;
    }

    for (int i = 0; i < PANIC_MAX_FRAMES; i++) {
        if (fp & 0x7UL) {
            printf("  #%-2d  [unaligned FP 0x%016lx]\n", i, fp);
            break;
        }

        unsigned long *frame = (unsigned long *)fp;
        unsigned long prev_fp = frame[0];
        unsigned long ret_addr = frame[1];

        printf("  #%-2d  0x%016lx\n", i, ret_addr);

        if (!prev_fp || prev_fp <= fp) {
            break;
        }

        fp = prev_fp;
    }
}

/* Prints the scheduler's view of the currently running task. */
static void panic_dump_task(void)
{
    printf("\n--- Current Task ---\n");

    struct task *t = sched_get_current();
    if (!t) {
        printf("  (no current task)\n");
        return;
    }

    printf("  Task ID  : %lu\n", t->id);
    printf("  PID      : %u\n", t->pid);
    printf("  State    : %d\n", (int)t->state);
    printf("  Stack    : 0x%016lx\n", (unsigned long)t->stack);
    printf("  TTBR0    : 0x%016lx\n", t->ttbr0);
}

/* Primary kernel error handler. */
void panic_full(const char *msg, const char *file, int line, unsigned long fp,
                struct exception_trap_frame *tf)
{
    disable_interrupts();

    /* Re-entrant guard: spin if diagnostic helpers fault. */
    if (kernel_panicked) {
        for (;;) {
            asm volatile("wfe");
        }
    }

    kernel_panicked = 1;
    asm volatile("dsb ish" ::: "memory");

    gic_send_panic_ipi();

    unsigned long uptime_ms = get_system_time();

    printf("\n           *** KERNEL PANIC ***           \n\n");
    printf("  Message  : %s\n", msg);
    printf("  Location : %s:%d\n", file, line);
    printf("  Uptime   : %lu ms\n", uptime_ms);

    panic_dump_task();

    if (tf) {
        panic_dump_tf_registers(tf);
        panic_backtrace(tf->x[29]);
    } else {
        panic_dump_live_registers();
        panic_backtrace(fp);
    }

    printf("\n--- All CPU cores halted ---\n\n");

    for (;;) {
        asm volatile("wfe");
    }
}
