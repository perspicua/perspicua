/*
 * test_user.c - The boot task driven as though it were a user process.
 *
 * Slot 0 is the kernel PCB and has no address space, so a syscall issued from
 * it refuses every pointer for want of a pgd, whatever the pointer's merits.
 */

#include <stddef.h>
#include <stdint.h>

#include "test.h"

#include "string.h"

#include "arch/exception.h"
#include "arch/irq.h"

#include "core/syscall.h"
#include "mm/addr.h"
#include "mm/asid.h"
#include "mm/mmu.h"
#include "mm/pmm.h"
#include "sched/process.h"
#include "sched/sched.h"

static struct exception_trap_frame test_tf;

int64_t test_syscall(uint64_t nr, const uint64_t *args)
{
    memset(&test_tf, 0, sizeof(test_tf));
    test_tf.x[8] = nr;
    for (int i = 0; i < 6; i++) {
        test_tf.x[i] = args[i];
    }
    syscall_handle(&test_tf);
    return (int64_t)test_tf.x[0];
}

unsigned long *test_borrow_user_pgd(void)
{
    unsigned long *pgd = mmu_create_user_pgd();
    if (!pgd) {
        return NULL;
    }

    struct process *kernel = process_table[0];

    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    kernel->user_pgd = pgd;
    kernel->va.count = 0;
    spin_unlock_irqrestore(&process_table_lock, flags);

    asid_get_active(&kernel->asid, &kernel->asid_generation);

    // The scheduler reinstalls a pid 0 task's own ttbr0 on every switch, so
    // this survives preemption and migration.
    flags = irq_save();
    sched_current_task()->ttbr0 = V2P(pgd) | asid_ttbr_field(kernel->asid);
    mmu_switch_user(pgd, kernel->asid);
    irq_restore(flags);

    return pgd;
}

void test_release_user_pgd(unsigned long *pgd)
{
    struct process *kernel = process_table[0];

    unsigned long flags = irq_save();
    sched_current_task()->ttbr0 = mmu_kernel_ttbr0();
    mmu_leave_user();
    irq_restore(flags);

    flags = spin_lock_irqsave(&process_table_lock);
    kernel->user_pgd = NULL;
    kernel->va.count = 0;
    spin_unlock_irqrestore(&process_table_lock, flags);

    asid_free(&kernel->asid, &kernel->asid_generation);
    mmu_destroy_user_pgd(pgd);
}

void *test_map_user_page(unsigned long *pgd, unsigned long va, unsigned long flags)
{
    void *page = pmm_alloc_page();
    if (!page) {
        return NULL;
    }

    if (mmu_user_map_page(pgd, va, V2P(page), flags) != 0) {
        pmm_free_page(page);
        return NULL;
    }
    return page;
}
