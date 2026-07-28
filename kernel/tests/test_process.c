/*
 * test_process.c - Tests for process table bookkeeping.
 *
 * These drive the slot allocator directly rather than through fork(), which
 * needs a live user process to copy. Every slot claimed here is released
 * before the suite returns.
 */

#include "test.h"

#include "sched/process.h"

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
        TEST_ASSERT("claim returns a slot", slot > 0);
        TEST_ASSERT("claimed slot is not EMPTY", process_table[slot].state != PROCESS_STATE_EMPTY);
        TEST_ASSERT("claimed slot carries its pid", process_table[slot].pid == (uint32_t)slot);

        // dirty it, release it, and take it again: the scan starts at 1, so the
        // same slot comes back and must have been cleared by the allocator
        process_table[slot].parent_pid = 0x5A5A;
        process_table[slot].pending_signals = 0xFFFF;
        process_table[slot].va.count = 7;
        process_table[slot].vaddr_code = 0xDEAD;
        process_table[slot].state = PROCESS_STATE_EMPTY;

        int again = process_test_claim_slot();
        TEST_ASSERT("same slot reclaimed", again == slot);
        TEST_ASSERT("reclaimed slot is not EMPTY",
                    process_table[slot].state != PROCESS_STATE_EMPTY);
        TEST_ASSERT("reclaimed slot was cleared", process_table[slot].parent_pid == 0
                                                      && process_table[slot].pending_signals == 0
                                                      && process_table[slot].va.count == 0
                                                      && process_table[slot].vaddr_code == 0);
        TEST_ASSERT("reclaimed slot has no stale task", process_table[slot].main_task == NULL
                                                            && process_table[slot].user_pgd == NULL
                                                            && process_table[slot].cwd == NULL);

        process_table[slot].state = PROCESS_STATE_EMPTY;
    }
    TEST_PASS("slot claim clears under the lock");

    // two claims must never hand out the same slot
    {
        int a = process_test_claim_slot();
        int b = process_test_claim_slot();

        TEST_ASSERT("first claim succeeds", a > 0);
        TEST_ASSERT("second claim succeeds", b > 0);
        TEST_ASSERT("claims are distinct", a != b);

        process_table[a].state = PROCESS_STATE_EMPTY;
        process_table[b].state = PROCESS_STATE_EMPTY;
    }
    TEST_PASS("claims are exclusive");

    TEST_SUITE_END("Process");
}
