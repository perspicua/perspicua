/*
 * test_process.c - Tests for process table bookkeeping.
 *
 * These drive the slot allocator directly rather than through fork(), which
 * needs a live user process to copy. TEST_ASSERT returns on failure, so each
 * block releases the slots it claimed *before* asserting: a leaked claim would
 * take PID 1 and stop init from loading later in the boot.
 */

#include "test.h"

#include "mm/addr.h"
#include "mm/mmu.h"
#include "sched/process.h"
#include "sched/sched.h"

static void release_slot(int slot)
{
    if (slot > 0) {
        process_table[slot].user_pgd = NULL;
        process_table[slot].state = PROCESS_STATE_EMPTY;
    }
}

void test_process(void)
{
    TEST_SUITE_BEGIN("Process");

    /*
     * A claimed slot must be zeroed and already non-EMPTY when it comes back.
     * PROCESS_STATE_EMPTY is 0, so clearing the PCB after the table lock is
     * dropped leaves the slot advertised as free for the length of a memset,
     * and a fork on another core claims it too.
     */
    {
        int slot = process_test_claim_slot();
        int claimed_not_empty = 0, carries_pid = 0, same_slot = 0;
        int recl_not_empty = 0, recl_cleared = 0, recl_no_task = 0;

        if (slot > 0) {
            claimed_not_empty = process_table[slot].state != PROCESS_STATE_EMPTY;
            carries_pid = process_table[slot].pid == (uint32_t)slot;

            // dirty it, release it, and take it again: the scan starts at 1, so
            // the same slot comes back and must have been cleared for us
            process_table[slot].parent_pid = 0x5A5A;
            process_table[slot].pending_signals = 0xFFFF;
            process_table[slot].va.count = 7;
            process_table[slot].vaddr_code = 0xDEAD;
            process_table[slot].state = PROCESS_STATE_EMPTY;

            int again = process_test_claim_slot();
            same_slot = again == slot;
            recl_not_empty = process_table[slot].state != PROCESS_STATE_EMPTY;
            recl_cleared =
                process_table[slot].parent_pid == 0 && process_table[slot].pending_signals == 0
                && process_table[slot].va.count == 0 && process_table[slot].vaddr_code == 0;
            recl_no_task = process_table[slot].main_task == NULL
                           && process_table[slot].user_pgd == NULL
                           && process_table[slot].cwd == NULL;

            release_slot(again);
            release_slot(slot);
        }

        TEST_ASSERT("claim returns a slot", slot > 0);
        TEST_ASSERT("claimed slot is not EMPTY", claimed_not_empty);
        TEST_ASSERT("claimed slot carries its pid", carries_pid);
        TEST_ASSERT("same slot reclaimed", same_slot);
        TEST_ASSERT("reclaimed slot is not EMPTY", recl_not_empty);
        TEST_ASSERT("reclaimed slot was cleared", recl_cleared);
        TEST_ASSERT("reclaimed slot has no stale task", recl_no_task);
    }
    TEST_PASS("slot claim clears under the lock");

    // two claims must never hand out the same slot
    {
        int a = process_test_claim_slot();
        int b = process_test_claim_slot();

        release_slot(a);
        release_slot(b);

        TEST_ASSERT("first claim succeeds", a > 0);
        TEST_ASSERT("second claim succeeds", b > 0);
        TEST_ASSERT("claims are distinct", a != b);
    }
    TEST_PASS("claims are exclusive");

    /*
     * process_exit clears user_pgd while its task is still runnable, so the
     * scheduler can reach a slot with no address space. It must fall back to
     * the kernel's TTBR0 rather than install one built from V2P(NULL).
     */
    {
        int slot = process_test_claim_slot();
        unsigned long *pgd = mmu_create_user_pgd();
        unsigned long live = 0, no_pgd = 0, zombie = 0, dead = 0;

        if (slot > 0 && pgd) {
            process_table[slot].state = PROCESS_STATE_RUNNING;
            process_table[slot].user_pgd = pgd;
            live = sched_test_task_ttbr0_for((uint32_t)slot);

            process_table[slot].user_pgd = NULL;
            no_pgd = sched_test_task_ttbr0_for((uint32_t)slot);

            process_table[slot].user_pgd = pgd;
            process_table[slot].state = PROCESS_STATE_ZOMBIE;
            zombie = sched_test_task_ttbr0_for((uint32_t)slot);

            process_table[slot].state = PROCESS_STATE_DEAD;
            dead = sched_test_task_ttbr0_for((uint32_t)slot);
        }

        release_slot(slot);
        if (pgd) {
            mmu_destroy_user_pgd(pgd);
        }

        TEST_ASSERT("ttbr0 test slot and pgd allocated", slot > 0 && pgd != NULL);
        TEST_ASSERT_EQ("running process uses its own pgd", live & MMU_PTE_ADDR_MASK, V2P(pgd));
        TEST_ASSERT_EQ("null pgd falls back to kernel ttbr0", no_pgd, mmu_kernel_ttbr0());
        TEST_ASSERT("null pgd is not V2P(NULL)", (no_pgd & MMU_PTE_ADDR_MASK) != V2P(NULL));
        TEST_ASSERT_EQ("zombie falls back to kernel ttbr0", zombie, mmu_kernel_ttbr0());
        TEST_ASSERT_EQ("dead falls back to kernel ttbr0", dead, mmu_kernel_ttbr0());
    }
    TEST_PASS("ttbr0 never built from a null pgd");

    TEST_SUITE_END("Process");
}
