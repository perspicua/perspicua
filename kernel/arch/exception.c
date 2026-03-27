/*
 * exception.c - AArch64 exception and interrupt handlers.
 *
 * This file handles synchronous exceptions (aborts, syscalls), IRQs, and
 * unhandled vectors. It integrates with the panic system to provide
 * detailed fault reports for kernel-space failures.
 */

#include "arch/exception.h"

#include "stdio.h"
#include "io.h"
#include "panic.h"

#include "arch/uaccess.h"

#include "core/timer.h"
#include "core/syscall.h"
#include "core/tty.h"
#include "mm/mmu.h"
#include "sched/sched.h"
#include "sched/process.h"
#include "driver/uart.h"
#include "driver/gic.h"

extern struct tty console_tty;
extern unsigned long __ex_table_start[];
extern unsigned long __ex_table_end[];

/* Exception Class values (EC field of ESR_EL1, bits [31:26]) */
#define EC_SVC              0x15
#define EC_INST_ABORT_LOWER 0x20
#define EC_INST_ABORT_SAME  0x21
#define EC_DATA_ABORT_LOWER 0x24
#define EC_DATA_ABORT_SAME  0x25

/* Fault Status Code masks (IFSC/DFSC, bits [5:0] of ESR_EL1) */
#define FSC_MASK           0x3F
#define FSC_TRANSLATION_L0 0x04
#define FSC_TRANSLATION_L3 0x07
#define FSC_PERMISSION_L1  0x0D
#define FSC_PERMISSION_L3  0x0F

/*
 * exception_fixup - Attempts to recover from a kernel-space fault using the
 * exception table.
 *
 * Returns 1 if a fixup was found and applied, 0 otherwise.
 */
int exception_fixup(struct exception_trap_frame *tf)
{
    unsigned long *p;
    for (p = __ex_table_start; p < __ex_table_end; p += 2) {
        if (tf->elr_el1 == p[0]) {
            tf->elr_el1 = p[1];
            return 1;
        }
    }
    return 0;
}

static const char *fsc_to_string(uint32_t fsc)
{
    switch (fsc) {
        case 0x00:
            return "Address size fault, level 0";
        case 0x01:
            return "Address size fault, level 1";
        case 0x02:
            return "Address size fault, level 2";
        case 0x03:
            return "Address size fault, level 3";
        case 0x04:
            return "Translation fault, level 0";
        case 0x05:
            return "Translation fault, level 1";
        case 0x06:
            return "Translation fault, level 2";
        case 0x07:
            return "Translation fault, level 3";
        case 0x09:
            return "Access flag fault, level 1";
        case 0x0A:
            return "Access flag fault, level 2";
        case 0x0B:
            return "Access flag fault, level 3";
        case 0x0D:
            return "Permission fault, level 1";
        case 0x0E:
            return "Permission fault, level 2";
        case 0x0F:
            return "Permission fault, level 3";
        case 0x10:
            return "Synchronous external abort, not on walk";
        case 0x14:
            return "Synchronous external abort, on walk";
        case 0x21:
            return "Alignment fault";
        default:
            return "Unknown fault";
    }
}

/*
 * handle_abort - Handles instruction and data abort exceptions.
 *
 * Dispatches user-space aborts to process termination and kernel aborts
 * to the panic system. Includes CoW handling for permission faults.
 */
static void handle_abort(struct exception_trap_frame *tf, uint32_t ec, uintptr_t esr)
{
    unsigned long far;
    asm volatile("mrs %0, far_el1" : "=r"(far));

    if (exception_fixup(tf)) {
        return;
    }

    uint32_t fsc = esr & FSC_MASK;
    int is_write =
        (ec == EC_DATA_ABORT_LOWER || ec == EC_DATA_ABORT_SAME) ? (int)((esr >> 6) & 1) : 0;
    int is_user = (ec == EC_INST_ABORT_LOWER || ec == EC_DATA_ABORT_LOWER);
    int is_inst = (ec == EC_INST_ABORT_LOWER || ec == EC_INST_ABORT_SAME);

    /* Attempt Copy-on-Write for user permission faults before failing */
    if (is_user && is_write && (fsc >= FSC_PERMISSION_L1 && fsc <= FSC_PERMISSION_L3)) {
        int pid = process_find_current();
        if (pid >= 0) {
            unsigned long *pgd = process_table[pid].user_pgd;
            if (mmu_handle_cow(pgd, far) == 0) {
                return;
            }
        }
    }

    if (is_user) {
        int pid = process_find_current();

        printk("\n[FAULT] %s abort in user process (PID %d)\n", is_inst ? "Instruction" : "Data",
               pid);

        if (far < 0x1000) {
            printk("  Type     : Likely NULL pointer dereference (FAR < 4K)\n");
        }

        printk("  FAR_EL1  : 0x%016lx\n", far);
        printk("  ELR_EL1  : 0x%016lx  (faulting PC)\n", tf->elr_el1);
        printk("  ESR_EL1  : 0x%016lx\n", (unsigned long)esr);
        printk("  FSC      : %s\n", fsc_to_string(fsc));
        printk("  Access   : %s\n", is_inst ? "execute" : (is_write ? "write" : "read"));

        if (pid >= 0) {
            printk("  Action   : killing PID %d\n", pid);
            struct task *curr = sched_get_current();
            if (curr && curr->pid == (uint32_t)pid) {
                process_exit(pid, 1);
                curr->state = SCHED_TASK_DEAD;
                schedule();
            } else {
                process_exit(pid, 1);
            }
        } else {
            printk("  Action   : no owning process found — halting\n");
            while (1) {
                asm volatile("wfe");
            }
        }
    } else {
        if (far < 0x1000) {
            pr_err("\n[KERNEL FAULT] NULL pointer dereference (FAR=0x%016lx)\n", far);
        } else {
            pr_err("\n[KERNEL FAULT] %s at FAR=0x%016lx, FSC=%s\n",
                   is_inst ? "Instruction abort" : (is_write ? "Write fault" : "Read fault"), far,
                   fsc_to_string(fsc));
        }
        PANIC_TF("Unrecoverable kernel memory abort", tf);
    }
}

/*
 * exception_unhandled_vector - Fallback for exception vectors with no handler.
 *
 * Triggered for FIQs, SError, or EL2 vectors taken at EL1.
 */
void exception_unhandled_vector(void)
{
    unsigned long esr, elr, far;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    asm volatile("mrs %0, elr_el1" : "=r"(elr));
    asm volatile("mrs %0, far_el1" : "=r"(far));

    printk("\n[UNHANDLED VECTOR]\n");
    printk("  ESR_EL1  : 0x%016lx\n", esr);
    printk("  ELR_EL1  : 0x%016lx\n", elr);
    printk("  FAR_EL1  : 0x%016lx\n", far);

    PANIC("Unhandled exception vector");
}

static unsigned int uart_irq_cached = 0;

/*
 * exception_irq_handler - Top-level IRQ dispatcher.
 *
 * Handles timer ticks, UART events, and inter-processor interrupts for panic
 * synchronization.
 */
void exception_irq_handler(void)
{
    /* Check for panic state before reading IAR to avoid locking up during shutdown */
    if (kernel_panicked) {
        disable_interrupts();
        for (;;) {
            asm volatile("wfe");
        }
    }

    if (uart_irq_cached == 0) {
        uart_irq_cached = uart_get_irq();
    }

    unsigned int iar = mmio_read(gic_c_iar);
    unsigned int irq_id = iar & 0x3FF;

    /* Spurious interrupt — EOIR write is forbidden */
    if (irq_id >= 1020) {
        return;
    }

    if (irq_id == 0) {
        /* SGI 0: panic IPI broadcast */
        mmio_write(gic_c_eoir, iar);
        disable_interrupts();
        for (;;) {
            asm volatile("wfe");
        }
    } else if (irq_id == GIC_TIMER_IRQ) {
        timer_interrupt_reset();
        mmio_write(gic_c_eoir, iar);
        schedule();
        return;
    } else if (irq_id == uart_irq_cached) {
        uint32_t mis = mmio_read(uart_mis);

        if (mis & (UART_MIS_RXMIS | UART_MIS_RTMIS)) {
            while (!(mmio_read(uart_fr) & UART_FR_RXFE)) {
                char c = (char)(mmio_read(uart_dr) & 0xFF);
                tty_handle_rx(&console_tty, c);
            }
        }

        if (mis & UART_MIS_TXMIS) {
            tty_handle_rx(&console_tty, 0);
        }

        uart_clear_interrupt(mis);
    }

    mmio_write(gic_c_eoir, iar);
}

/*
 * exception_sync_handler - Top-level synchronous exception dispatcher.
 *
 * Routes SVC calls to the syscall handler and aborts to the memory fault handler.
 */
void exception_sync_handler(struct exception_trap_frame *tf)
{
    uintptr_t esr;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));

    uint32_t ec = (uint32_t)((esr >> 26) & 0x3F);

    switch (ec) {
        case EC_SVC:
            syscall_handle(tf);
            break;

        case EC_INST_ABORT_LOWER:
        case EC_INST_ABORT_SAME:
        case EC_DATA_ABORT_LOWER:
        case EC_DATA_ABORT_SAME:
            handle_abort(tf, ec, esr);
            break;

        default: {
            unsigned long far;
            asm volatile("mrs %0, far_el1" : "=r"(far));

            pr_err("\n[KERNEL FAULT] Unhandled synchronous exception\n");
            printk("  EC       : 0x%02x\n", (unsigned int)ec);
            printk("  ESR_EL1  : 0x%016lx\n", (unsigned long)esr);
            printk("  FAR_EL1  : 0x%016lx\n", far);
            printk("  ELR_EL1  : 0x%016lx\n", tf->elr_el1);

            PANIC_TF("Unhandled exception class", tf);
        }
    }
}
