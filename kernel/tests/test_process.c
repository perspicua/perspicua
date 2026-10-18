/*
 * test_process.c - Process slots and the user VA allocator; a leaked slot would take init's PID 1.
 */

#include "test.h"

#include <stddef.h>
#include <stdint.h>

#include "string.h"

#include "uapi/errno.h"
#include "uapi/mman.h"
#include "uapi/syscalls.h"

#include "fs/vfs.h"
#include "mm/addr.h"
#include "mm/mmu.h"
#include "sched/process.h"
#include "sched/sched.h"

#define MMAP_FILE "/tmmap.tmp"

static int64_t call_mmap(size_t length, int flags, int fd)
{
    // The address hint is unused.
    uint64_t args[6] = {0, length, PROT_READ | PROT_WRITE, (uint64_t)flags, (uint64_t)(int64_t)fd,
                        0};
    return test_syscall(SYS_MMAP, args);
}

static void release_slot(int slot)
{
    if (slot > 0) {
        process_table[slot]->user_pgd = NULL;
        process_test_release_slot((uint32_t)slot);
    }
}

void test_process(void)
{
    TEST_SUITE_BEGIN("Process");

    // A free slot is NULL, so a claimed slot is published only once its PCB is fully built.
    {
        int slot = process_test_claim_slot();
        int published = 0, carries_pid = 0, is_running = 0, same_slot = 0;
        int freed_is_null = 0, recl_cleared = 0, recl_no_task = 0;

        if (slot > 0) {
            published = process_table[slot] != NULL;
            carries_pid = process_table[slot]->pid == (uint32_t)slot;
            is_running = process_table[slot]->state == PROCESS_STATE_RUNNING;

            // The scan starts at 1, so a dirtied and released slot comes back and must be clean.
            process_table[slot]->parent_pid = 0x5A5A;
            process_table[slot]->pending_signals = 0xFFFF;
            process_table[slot]->va.count = 7;
            process_table[slot]->vaddr_code = 0xDEAD;
            process_test_release_slot((uint32_t)slot);
            freed_is_null = process_table[slot] == NULL;

            int again = process_test_claim_slot();
            same_slot = again == slot;
            recl_cleared =
                process_table[slot]->parent_pid == 0 && process_table[slot]->pending_signals == 0
                && process_table[slot]->va.count == 0 && process_table[slot]->vaddr_code == 0;
            recl_no_task = process_table[slot]->main_task == NULL
                           && process_table[slot]->user_pgd == NULL
                           && process_table[slot]->cwd == NULL;

            release_slot(again);
        }

        TEST_ASSERT("claim returns a slot", slot > 0);
        TEST_ASSERT("claimed slot is published", published);
        TEST_ASSERT("claimed slot carries its pid", carries_pid);
        TEST_ASSERT("claimed slot is RUNNING", is_running);
        TEST_ASSERT("released slot reads as free", freed_is_null);
        TEST_ASSERT("same slot reclaimed", same_slot);
        TEST_ASSERT("reclaimed slot was cleared", recl_cleared);
        TEST_ASSERT("reclaimed slot has no stale task", recl_no_task);
    }

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

    // A runnable task with no user_pgd, as after process_exit, gets the kernel TTBR0.
    {
        int slot = process_test_claim_slot();
        unsigned long *pgd = mmu_create_user_pgd();
        unsigned long live = 0, no_pgd = 0, zombie = 0, dead = 0;

        if (slot > 0 && pgd) {
            process_table[slot]->state = PROCESS_STATE_RUNNING;
            process_table[slot]->user_pgd = pgd;
            live = sched_test_task_ttbr0_for((uint32_t)slot);

            process_table[slot]->user_pgd = NULL;
            no_pgd = sched_test_task_ttbr0_for((uint32_t)slot);

            process_table[slot]->user_pgd = pgd;
            process_table[slot]->state = PROCESS_STATE_ZOMBIE;
            zombie = sched_test_task_ttbr0_for((uint32_t)slot);

            process_table[slot]->state = PROCESS_STATE_DEAD;
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

    // mmap never returns a region with nothing behind it: a file with no mmap op is refused.
    {
        unsigned long *pgd = test_borrow_user_pgd();
        int fd = vfs_open(MMAP_FILE, O_RDWR | O_CREAT);

        int64_t file_backed = 0, anon = 0, anon_with_fd = 0, bad_fd = 0, no_backing = 0;

        if (pgd && fd >= 0) {
            file_backed = call_mmap(PAGE_SIZE, 0, fd);              // no ->mmap op
            anon_with_fd = call_mmap(PAGE_SIZE, MAP_ANONYMOUS, fd); // contradictory
            bad_fd = call_mmap(PAGE_SIZE, 0, VFS_MAX_FDS + 5);
            no_backing = call_mmap(PAGE_SIZE, 0, -1); // neither file nor anonymous
            anon = call_mmap(PAGE_SIZE, MAP_ANONYMOUS, -1);
        }

        if (fd >= 0) {
            vfs_close(fd);
            vfs_unlink(MMAP_FILE);
        }
        if (pgd) {
            test_release_user_pgd(pgd);
        }

        // The errno is the assertion: -1 reaches the caller as EPERM.
        TEST_ASSERT("mmap test setup", pgd != NULL && fd >= 0);
        TEST_ASSERT("file with no mmap op is refused", file_backed == -ENODEV);
        TEST_ASSERT("anonymous with a descriptor is refused", anon_with_fd == -EBADF);
        TEST_ASSERT("out-of-range descriptor is refused", bad_fd == -EBADF);
        TEST_ASSERT("neither file nor anonymous is refused", no_backing == -EBADF);
        TEST_ASSERT("anonymous mapping succeeds", anon > 0);
    }

    // Address space a mapping releases is available to the next, so map-unmap loops do not leak it.
    {
        static struct va_allocator va;
        memset(&va, 0, sizeof(va));

        uintptr_t a = process_va_alloc(&va, 1);
        uintptr_t b = process_va_alloc(&va, 1);
        uintptr_t c = process_va_alloc(&va, 1);

        TEST_ASSERT("first allocation starts at the base", a == USER_VA_BASE);
        TEST_ASSERT("allocations are distinct and ordered", a < b && b < c);
        TEST_ASSERT("allocations do not overlap", b >= a + PAGE_SIZE && c >= b + PAGE_SIZE);

        // the hole left by b must be handed out again
        process_va_free(&va, b);
        uintptr_t reused = process_va_alloc(&va, 1);
        TEST_ASSERT_EQ("a released range is reused", (long)reused, (long)b);

        // a request too big for the hole goes after everything instead
        process_va_free(&va, reused);
        uintptr_t big = process_va_alloc(&va, 4);
        TEST_ASSERT("an oversized request skips a too-small gap", big >= c + PAGE_SIZE);

        // and the small hole is still there for something that fits
        uintptr_t small = process_va_alloc(&va, 1);
        TEST_ASSERT_EQ("the small gap is still usable", (long)small, (long)b);

        process_va_free(&va, a);
        process_va_free(&va, c);
        process_va_free(&va, big);
        process_va_free(&va, small);
        TEST_ASSERT_EQ("every region released", (long)va.count, 0);

        // an allocation larger than the whole range cannot be satisfied
        TEST_ASSERT_EQ("an impossible size is refused",
                       (long)process_va_alloc(&va, USER_VA_LIMIT / PAGE_SIZE), 0);
        TEST_ASSERT_EQ("zero pages is refused", (long)process_va_alloc(&va, 0), 0);
    }

    // a free slot whose number a live process still uses as its group is not handed out
    {
        int leader = process_test_claim_slot();
        int member = process_test_claim_slot();
        int while_named = -1, once_free = -1;
        if (leader > 0 && member > 0) {
            process_table[member]->pgid = (uint32_t)leader;
            process_test_release_slot((uint32_t)leader);
            while_named = process_test_claim_slot();

            process_table[member]->pgid = 0;
            once_free = process_test_claim_slot();
        }
        TEST_ASSERT("slots claimed", leader > 0 && member > 0);
        TEST_ASSERT("a pid naming a live group is skipped",
                    while_named > 0 && while_named != leader);
        TEST_ASSERT_EQ("the pid comes back once no group uses it", once_free, leader);
        release_slot(once_free);
        release_slot(while_named);
        release_slot(member);
    }

    // a full table makes fork fail with EAGAIN, the error a caller retries on
    {
        static int claimed[PROCESS_TABLE_SIZE];
        int n = 0, last = 0;
        while (n < PROCESS_TABLE_SIZE && (last = process_test_claim_slot()) > 0) {
            claimed[n++] = last;
        }
        TEST_ASSERT_EQ("a full table reports EAGAIN", last, -EAGAIN);
        while (n > 0) {
            release_slot(claimed[--n]);
        }
    }

    TEST_SUITE_END("Process");
}
