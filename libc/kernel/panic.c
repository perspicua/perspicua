/*
 * panic.c - Implementation of the kernel panic handler.
 *
 * Halts all CPU cores on an unrecoverable error and emits a diagnostic
 * report containing register state, task info, and stack backtrace.
 */

#include "panic.h"

#include <stdarg.h>
#include <stddef.h>

#include "stdio.h"

#include "arch/exception.h"

#include "arch/irq.h"
#include "core/timer.h"
#include "debug/kdb.h"
#include "driver/gic.h"
#include "sched/sched.h"

// Global state for core synchronization during panic.
volatile int kernel_panicked = 0;

#define PANIC_MAX_FRAMES 16

/*
 * panic_printf - Formatted output for the panic report.
 *
 * Not printf: that takes printf_lock, and a panic triggered while this core
 * already holds it (mid printk/printf elsewhere) would spin on a lock it
 * owns itself. vprintf does the formatting without touching it -- same
 * reasoning as kdb_printf, which this report hands off to once KDB starts.
 */
__attribute__((format(printf, 1, 2))) static void panic_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

// Decodes Exception Class (EC) for diagnostic output.
static void print_ec(unsigned int ec)
{
    switch (ec) {
        case 0x01:
            panic_printf("  [WFI/WFE]\n");
            break;
        case 0x15:
            panic_printf("  [SVC AArch64]\n");
            break;
        case 0x20:
            panic_printf("  [Inst Abort, lower EL]\n");
            break;
        case 0x21:
            panic_printf("  [Inst Abort, same EL]\n");
            break;
        case 0x24:
            panic_printf("  [Data Abort, lower EL]\n");
            break;
        case 0x25:
            panic_printf("  [Data Abort, same EL]\n");
            break;
        case 0x2C:
            panic_printf("  [SP Alignment Fault]\n");
            break;
        case 0x30:
            panic_printf("  [FP Exception]\n");
            break;
        case 0x3C:
            panic_printf("  [BRK instruction]\n");
            break;
        default:
            panic_printf("  [EC=0x%02x]\n", ec);
            break;
    }
}

static void panic_dump_tf_registers(struct exception_trap_frame *tf)
{
    panic_printf("\n--- Registers (from exception trap frame) ---\n");

    for (int i = 0; i < 28; i += 2) {
        panic_printf("  x%-2d: 0x%016lx   x%-2d: 0x%016lx\n", i, tf->x[i], i + 1, tf->x[i + 1]);
    }
    panic_printf("  x29: 0x%016lx   x30: 0x%016lx\n", tf->x[29], tf->x30);

    panic_printf("\n--- System Registers (from trap frame) ---\n");
    panic_printf("  ELR_EL1  : 0x%016lx  (faulting PC)\n", tf->elr_el1);
    panic_printf("  SP_EL0   : 0x%016lx\n", tf->sp_el0);
    panic_printf("  SPSR_EL1 : 0x%016lx\n", tf->spsr_el1);

    unsigned long esr, far_reg;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    asm volatile("mrs %0, far_el1" : "=r"(far_reg));

    panic_printf("  ESR_EL1  : 0x%016lx", esr);
    print_ec((unsigned int)((esr >> 26) & 0x3F));
    panic_printf("  FAR_EL1  : 0x%016lx\n", far_reg);
}

// Captures and prints key AArch64 registers from EL1 context.
static void panic_dump_live_registers(void)
{
    unsigned long sp, lr, spsr, esr, far_reg;

    asm volatile("mov %0, sp" : "=r"(sp));
    asm volatile("mov %0, x30" : "=r"(lr));
    asm volatile("mrs %0, spsr_el1" : "=r"(spsr));
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    asm volatile("mrs %0, far_el1" : "=r"(far_reg));

    panic_printf("\n--- Registers (live EL1 snapshot) ---\n");
    panic_printf("  SP       : 0x%016lx\n", sp);
    panic_printf("  LR (x30) : 0x%016lx\n", lr);
    panic_printf("  SPSR_EL1 : 0x%016lx\n", spsr);
    panic_printf("  ESR_EL1  : 0x%016lx", esr);
    print_ec((unsigned int)((esr >> 26) & 0x3F));
    panic_printf("  FAR_EL1  : 0x%016lx\n", far_reg);
}

// Walks the AArch64 frame pointer chain.
static void panic_backtrace(unsigned long fp)
{
    panic_printf("\n--- Stack Trace ---\n");

    if (!fp) {
        panic_printf("  (frame pointer is NULL)\n");
        return;
    }

    for (int i = 0; i < PANIC_MAX_FRAMES; i++) {
        if (fp & 0x7UL) {
            panic_printf("  #%-2d  [unaligned FP 0x%016lx]\n", i, fp);
            break;
        }

        unsigned long *frame = (unsigned long *)fp;
        unsigned long prev_fp = frame[0];
        unsigned long ret_addr = frame[1];

        unsigned long offset = 0;
        const char *sym_name = panic_resolve_symbol(ret_addr, &offset);

        if (sym_name) {
            panic_printf("  #%-2d  0x%016lx <%s+0x%lx>\n", i, ret_addr, sym_name, offset);
        } else {
            panic_printf("  #%-2d  0x%016lx\n", i, ret_addr);
        }

        if (!prev_fp || prev_fp <= fp) {
            break;
        }

        fp = prev_fp;
    }
}

// Prints the scheduler's view of the currently running task.
static void panic_dump_task(void)
{
    panic_printf("\n--- Current Task ---\n");

    struct task *t = sched_current_task();
    if (!t) {
        panic_printf("  (no current task)\n");
        return;
    }

    panic_printf("  Task ID  : %lu\n", t->id);
    panic_printf("  PID      : %u\n", t->pid);
    panic_printf("  State    : %d\n", (int)t->state);
    panic_printf("  Stack    : 0x%016lx\n", (unsigned long)t->stack);
    panic_printf("  TTBR0    : 0x%016lx\n", t->ttbr0);
}

void panic_full(const char *msg, const char *file, int line, unsigned long fp,
                struct exception_trap_frame *tf)
{
    disable_interrupts();

    // Re-entrant guard: spin if diagnostic helpers fault.
    if (kernel_panicked) {
        for (;;) {
            asm volatile("wfe");
        }
    }

    kernel_panicked = 1;
    asm volatile("dsb ish" ::: "memory");

    gic_send_panic_ipi();

    unsigned long uptime_ms = timer_get_system_time();

    panic_printf("\n           *** KERNEL PANIC ***           \n\n");
    panic_printf("  Message  : %s\n", msg);
    panic_printf("  Location : %s:%d\n", file, line);
    panic_printf("  Uptime   : %lu ms\n", uptime_ms);

    panic_dump_task();

    if (tf) {
        panic_dump_tf_registers(tf);
        panic_backtrace(tf->x[29]);
    } else {
        panic_dump_live_registers();
        panic_backtrace(fp);
    }

#ifdef CONFIG_KDB_ON_PANIC
    panic_printf("\n--- Entering KDB (type 'continue' to halt) ---\n\n");

    if (tf) {
        kdb_enter_tf("kernel panic", tf);
    } else {
        kdb_enter("kernel panic");
    }
#else
    // No watchdog driver exists yet to reboot from (order.txt Phase 0 item
    // 14), so an unattended board halts here instead of hanging forever
    // waiting for a keystroke that will never come.
    panic_printf("\n--- CONFIG_KDB_ON_PANIC is off; halting ---\n\n");
#endif

    panic_printf("\n--- All CPU cores halted ---\n\n");

    for (;;) {
        asm volatile("wfe");
    }
}
