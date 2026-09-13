/*
 * exception.c - AArch64 exception and interrupt handlers.
 */

#include "arch/exception.h"

#include "stdio.h"
#include "io.h"
#include "panic.h"

#include "arch/uaccess.h"

#include "arch/irq.h"
#include "core/timer.h"
#include "core/syscall.h"
#include "mm/mmu.h"
#include "mm/addr.h"
#include "sched/sched.h"
#include "sched/process.h"
#include "driver/gic.h"

extern unsigned long __ex_table_start[];
extern unsigned long __ex_table_end[];

// Exception Class values (EC field of ESR_EL1, bits [31:26])
#define EC_SVC              0x15
#define EC_INST_ABORT_LOWER 0x20
#define EC_INST_ABORT_SAME  0x21
#define EC_DATA_ABORT_LOWER 0x24
#define EC_DATA_ABORT_SAME  0x25

// Fault Status Code masks (IFSC/DFSC, bits [5:0] of ESR_EL1)
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
static void handle_abort(struct exception_trap_frame *tf, uint32_t ec, uintptr_t esr,
                         unsigned long far)
{
    uint32_t fsc = esr & FSC_MASK;
    int is_write =
        (ec == EC_DATA_ABORT_LOWER || ec == EC_DATA_ABORT_SAME) ? (int)((esr >> 6) & 1) : 0;
    int is_user_fault = (ec == EC_INST_ABORT_LOWER || ec == EC_DATA_ABORT_LOWER);
    int is_inst = (ec == EC_INST_ABORT_LOWER || ec == EC_INST_ABORT_SAME);

    /* Attempt Copy-on-Write for user permission faults before failing.
     * We also check if it's a kernel access to user memory (far < KERNEL_VMA).
     */
    if (is_write && (fsc >= FSC_PERMISSION_L1 && fsc <= FSC_PERMISSION_L3) && (far < KERNEL_VMA)) {
        struct process *p = process_current();
        if (p && p->user_pgd) {
            if (mmu_handle_cow(p->user_pgd, far) == 0) {
                return;
            }
        }
    }

    if (exception_fixup(tf)) {
        return;
    }

    if (is_user_fault) {
        int pid = process_current_pid();

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
            struct task *curr = sched_current_task();
            if (curr && curr->pid == (uint32_t)pid) {
                process_exit(pid, 1);
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

/*
 * exception_irq_handler - Top-level IRQ dispatcher.
 *
 * Owns the GIC acknowledge/end-of-interrupt pair and the panic IPI; everything
 * else is a handler claimed through request_irq. A handler returning
 * IRQ_HANDLED_RESCHED is rescheduled here, after the line is closed, because
 * sched_schedule() does not return.
 */
void exception_irq_handler(void)
{
    // Check for panic state before reading IAR to avoid locking up during shutdown
    if (kernel_panicked) {
        disable_interrupts();
        for (;;) {
            asm volatile("wfe");
        }
    }

    unsigned int iar = mmio_read(gic_c_iar);
    unsigned int irq_id = iar & 0x3FF;

    // Spurious interrupt — EOIR write is forbidden
    if (irq_id >= 1020) {
        return;
    }

    if (irq_id == 0) {
        // SGI 0: panic IPI broadcast
        mmio_write(gic_c_eoir, iar);
        disable_interrupts();
        for (;;) {
            asm volatile("wfe");
        }
    }

    irq_result_t res = irq_dispatch(irq_id);

    mmio_write(gic_c_eoir, iar);

    if (res == IRQ_HANDLED_RESCHED) {
        sched_schedule();
    }
}

/*
 * exception_sync_handler - Top-level synchronous exception dispatcher.
 *
 * Routes SVC calls to the syscall handler and aborts to the memory fault handler.
 */
void exception_sync_handler(struct exception_trap_frame *tf)
{
    uintptr_t esr;
    unsigned long far;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    asm volatile("mrs %0, far_el1" : "=r"(far));

    /*
     * Syndrome captured, so this core can take exceptions again. ESR_EL1 and
     * FAR_EL1 are not banked: unmasking before reading them lets a preempting
     * task's own abort overwrite both, and this handler then resolves the fault
     * for someone else's address -- leaving the real faulting page unresolved.
     *
     * Exceptions from EL1 stay masked: a kernel fault can be taken inside a
     * critical section, where preemption would not be safe.
     */
    if ((tf->spsr_el1 & 0xF) == 0) {
        asm volatile("msr daifclr, #2" : : : "memory");
    }

    uint32_t ec = (uint32_t)((esr >> 26) & 0x3F);

    switch (ec) {
        case EC_SVC:
            syscall_handle(tf);
            break;

        case EC_INST_ABORT_LOWER:
        case EC_INST_ABORT_SAME:
        case EC_DATA_ABORT_LOWER:
        case EC_DATA_ABORT_SAME:
            handle_abort(tf, ec, esr, far);
            break;

        default: {
            pr_err("\n[KERNEL FAULT] Unhandled synchronous exception\n");
            printk("  EC       : 0x%02x\n", (unsigned int)ec);
            printk("  ESR_EL1  : 0x%016lx\n", (unsigned long)esr);
            printk("  FAR_EL1  : 0x%016lx\n", far);
            printk("  ELR_EL1  : 0x%016lx\n", tf->elr_el1);

            PANIC_TF("Unhandled exception class", tf);
        }
    }
}
