#include "arch/exception.h"
#include "driver/uart.h"
#include "driver/gic.h"
#include "stdio.h"
#include "panic.h"
#include "timer.h"
#include "sched.h"
#include "process.h"
#include "syscall.h"
#include "tty.h"
#include "arch/uaccess.h"
#include "mmu.h"

extern struct tty console_tty;

extern unsigned long __ex_table_start[];
extern unsigned long __ex_table_end[];

int exception_fixup(struct exception_trap_frame* tf)
{
    unsigned long* p;
    for (p = __ex_table_start; p < __ex_table_end; p += 2)
    {
        if (tf->elr_el1 == p[0])
        {
            tf->elr_el1 = p[1];
            return 1;
        }
    }
    return 0;
}

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

static const char* fsc_to_string(uint32_t fsc)
{
    switch (fsc)
    {
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

static void dump_regs(struct exception_trap_frame* tf)
{
    printf("Register Dump:\n");
    for (int i = 0; i < 30; i += 2)
    {
        printf("  x%d: %p  x%d: %p\n", i, (void*)tf->x[i], i + 1, (void*)tf->x[i + 1]);
    }
    printf("  x30: %p  sp_el0: %p\n", (void*)tf->x30, (void*)tf->sp_el0);
    printf("  elr_el1: %p  spsr_el1: %p\n", (void*)tf->elr_el1, (void*)tf->spsr_el1);
}

static void handle_abort(struct exception_trap_frame* tf, uint32_t ec, uintptr_t esr)
{
    unsigned long far;
    asm volatile("mrs %0, far_el1" : "=r"(far));

    if (exception_fixup(tf))
    {
        return;
    }

    uint32_t fsc       = esr & FSC_MASK;
    int is_write       = (ec == EC_DATA_ABORT_LOWER || ec == EC_DATA_ABORT_SAME) ? (int)((esr >> 6) & 1) : 0;
    int is_user        = (ec == EC_INST_ABORT_LOWER || ec == EC_DATA_ABORT_LOWER);
    int is_inst        = (ec == EC_INST_ABORT_LOWER || ec == EC_INST_ABORT_SAME);
    int is_translation = (fsc >= FSC_TRANSLATION_L0 && fsc <= FSC_TRANSLATION_L3);

    if (is_user && is_write && (fsc == FSC_PERMISSION_L3))
    {
        int pid = process_find_current();
        if (pid >= 0)
        {
            unsigned long* pgd = process_table[pid].user_pgd;
            if (mmu_handle_cow(pgd, far) == 0)
            {
                return;
            }
        }
    }

    (void)is_translation;  // demand paging hook (ca tot ma batea la cap riciu cu demand paging)

    if (is_user)
    {
        int pid = process_find_current();

        printf("\n[FAULT] %s abort in user process (PID %d)\n", is_inst ? "Instruction" : "Data", pid);
        if (far < 0x1000)
            printf("  Type     : NULL Pointer Dereference\n");
        printf("  FAR_EL1  : %p\n", (void*)far);
        printf("  ELR_EL1  : %p\n", (void*)tf->elr_el1);
        printf("  ESR_EL1  : %lx\n", (unsigned long)esr);
        printf("  Fault    : %s\n", fsc_to_string(fsc));
        printf("  Access   : %s\n", is_inst ? "execute" : (is_write ? "write" : "read"));

        dump_regs(tf);

        if (pid >= 0)
        {
            printf("  Action   : killing PID %d\n", pid);
            struct task* curr = sched_get_current();
            if (curr && curr->pid == (uint32_t)pid)
            {
                curr->state = SCHED_TASK_DEAD;
                schedule();
            }
            else
            {
                process_exit((uint32_t)pid, 1);
            }
        }
        else
        {
            printf("  Action   : no owning process found, halting\n");
        }

        while (1)
            asm volatile("wfe");
    }
    else
    {
        struct task* curr = sched_get_current();
        int pid           = curr ? (int)curr->pid : -1;
        unsigned long tid = curr ? curr->id : 0;

        printf("\n[KERNEL PANIC] %s abort in kernel! (PID %d, TID %lu)\n", is_inst ? "Instruction" : "Data", pid, tid);
        if (far < 0x1000)
            printf("  Type     : NULL Pointer Dereference\n");
        printf("  FAR_EL1  : %p\n", (void*)far);
        printf("  ELR_EL1  : %p\n", (void*)tf->elr_el1);
        printf("  ESR_EL1  : %lx\n", (unsigned long)esr);
        printf("  Fault    : %s\n", fsc_to_string(fsc));
        printf("  Access   : %s\n", is_inst ? "execute" : (is_write ? "write" : "read"));

        dump_regs(tf);
        PANIC("Unrecoverable kernel abort");
    }
}

void exception_unhandled_vector(void)
{
    unsigned int esr;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));

    unsigned long elr;
    asm volatile("mrs %0, elr_el1" : "=r"(elr));

    unsigned long far;
    asm volatile("mrs %0, far_el1" : "=r"(far));

    printf("An unhandled exception occurred!\n");
    printf("FAR_EL1 (Faulting Address): 0x%lx\n", far);
    printf("ESR_EL1 (Reason)  : 0x%lx\n", (unsigned long)esr);
    printf("ELR_EL1 (Address) : 0x%lx\n", elr);
    printf("System halted.\n");

    while (1)
    {
        asm volatile("wfe");
    }
}

static unsigned int uart_irq_cached = 0;

void exception_irq_handler(void)
{
    // check before even reading IAR — a panic IPI may have woken us
    if (kernel_panicked)
    {
        disable_interrupts();
        for (;;)
            asm volatile("wfe");
    }

    if (uart_irq_cached == 0)
        uart_irq_cached = uart_get_irq();

    unsigned int iar    = mmio_read(gic_c_iar);
    unsigned int irq_id = iar & 0x3FF;

    if (irq_id >= 1020)  // spurious interrupt — do NOT write EOIR
        return;

    if (irq_id == 0)  // SGI 0: panic IPI from another core
    {
        mmio_write(gic_c_eoir, iar);
        disable_interrupts();
        for (;;)
            asm volatile("wfe");
    }
    else if (irq_id == GIC_TIMER_IRQ)
    {
        timer_interrupt_reset();
        mmio_write(gic_c_eoir, iar);
        schedule();
        return;
    }
    else if (irq_id == uart_irq_cached)
    {
        uint32_t mis = mmio_read(uart_mis);

        // Handle RX and RX Timeout
        if (mis & (UART_MIS_RXMIS | UART_MIS_RTMIS))
        {
            while (!(mmio_read(uart_fr) & UART_FR_RXFE))
            {
                char c = (char)(mmio_read(uart_dr) & 0xFF);
                tty_handle_rx(&console_tty, c);
            }
        }

        // Handle TX
        if (mis & UART_MIS_TXMIS)
        {
            tty_handle_rx(&console_tty, 0);  // Trigger pump_tx
        }

        uart_clear_interrupt(mis);
    }

    mmio_write(gic_c_eoir, iar);
}

void exception_sync_handler(struct exception_trap_frame* tf)
{
    uintptr_t esr;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));

    uint32_t ec = (esr >> 26) & 0b111111;

    switch (ec)
    {
    case EC_SVC:
    {
        syscall_handle(tf);
        break;
    }
    case EC_INST_ABORT_LOWER:
    case EC_INST_ABORT_SAME:
    case EC_DATA_ABORT_LOWER:
    case EC_DATA_ABORT_SAME:
        handle_abort(tf, ec, esr);
        break;
    default:
    {
        unsigned long far;
        asm volatile("mrs %0, far_el1" : "=r"(far));

        printf("\n[KERNEL PANIC] Unhandled synchronous exception!\n");
        printf("  EC       : 0x%lx\n", (unsigned long)ec);
        printf("  FAR_EL1  : 0x%lx\n", far);
        printf("  ESR_EL1  : 0x%lx\n", (unsigned long)esr);
        printf("  ELR_EL1  : 0x%lx\n", tf->elr_el1);
        PANIC("Unhandled exception class");
    }
    }
}
