/*
 * test_process.c - Tests for process table bookkeeping.
 *
 * These drive the slot allocator directly rather than through fork(), which
 * needs a live user process to copy. TEST_ASSERT returns on failure, so each
 * block releases the slots it claimed *before* asserting: a leaked claim would
 * take PID 1 and stop init from loading later in the boot.
 */

#include "test.h"

#include "string.h"

#include "uapi/mman.h"
#include "uapi/syscalls.h"

#include "arch/exception.h"

#include "core/syscall.h"
#include "fs/vfs.h"
#include "mm/addr.h"
#include "mm/mmu.h"
#include "sched/process.h"
#include "sched/sched.h"

#define MMAP_FILE "/tmmap.tmp"

/* Driving SYS_MMAP needs a trap frame; it is 800 bytes, so keep it off the stack. */
static struct exception_trap_frame mmap_tf;

static int64_t call_mmap(size_t length, int flags, int fd)
{
    memset(&mmap_tf, 0, sizeof(mmap_tf));
    mmap_tf.x[8] = SYS_MMAP;
    mmap_tf.x[0] = 0; /* addr hint, unused */
    mmap_tf.x[1] = length;
    mmap_tf.x[2] = PROT_READ | PROT_WRITE;
    mmap_tf.x[3] = (uint64_t)flags;
    mmap_tf.x[4] = (uint64_t)(int64_t)fd;
    syscall_handle(&mmap_tf);
    return (int64_t)mmap_tf.x[0];
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

    /*
     * A free slot is a null pointer, so a claim is only visible once the PCB
     * behind it is fully built: there is no window in which the slot is
     * published but still carries another process's state.
     */
    {
        int slot = process_test_claim_slot();
        int published = 0, carries_pid = 0, is_running = 0, same_slot = 0;
        int freed_is_null = 0, recl_cleared = 0, recl_no_task = 0;

        if (slot > 0) {
            published = process_table[slot] != NULL;
            carries_pid = process_table[slot]->pid == (uint32_t)slot;
            is_running = process_table[slot]->state == PROCESS_STATE_RUNNING;

            // dirty it, release it, and take it again: the scan starts at 1, so
            // the same slot comes back and must be a clean PCB
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
    TEST_PASS("a slot is free exactly when it is null");

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
    TEST_PASS("ttbr0 never built from a null pgd");

    /*
     * mmap must never hand back a region with nothing behind it. Only the
     * framebuffer implements the vnode mmap operation, so a mapping of an
     * ordinary file used to succeed and return an address that faulted on
     * first touch.
     *
     * Driving this needs an address space, and the test task is pid 0, which
     * has none: lend it one for the duration and take it back before asserting.
     */
    {
        unsigned long *pgd = mmu_create_user_pgd();
        int fd = vfs_open(MMAP_FILE, VFS_O_RDWR | VFS_O_CREAT);

        int64_t file_backed = 0, anon = 0, anon_with_fd = 0, bad_fd = 0, no_backing = 0;

        if (pgd && fd >= 0) {
            process_table[0]->user_pgd = pgd;
            process_table[0]->va.count = 0;

            file_backed = call_mmap(PAGE_SIZE, 0, fd);              // no ->mmap op
            anon_with_fd = call_mmap(PAGE_SIZE, MAP_ANONYMOUS, fd); // contradictory
            bad_fd = call_mmap(PAGE_SIZE, 0, VFS_MAX_FDS + 5);
            no_backing = call_mmap(PAGE_SIZE, 0, -1); // neither file nor anonymous
            anon = call_mmap(PAGE_SIZE, MAP_ANONYMOUS, -1);

            process_table[0]->user_pgd = NULL;
            process_table[0]->va.count = 0;
        }

        if (fd >= 0) {
            vfs_close(fd);
            vfs_unlink(MMAP_FILE);
        }
        if (pgd) {
            mmu_destroy_user_pgd(pgd);
        }

        TEST_ASSERT("mmap test setup", pgd != NULL && fd >= 0);
        TEST_ASSERT("file with no mmap op is refused",
                    file_backed == (int64_t)(uintptr_t)MAP_FAILED);
        TEST_ASSERT("anonymous with a descriptor is refused",
                    anon_with_fd == (int64_t)(uintptr_t)MAP_FAILED);
        TEST_ASSERT("out-of-range descriptor is refused", bad_fd == (int64_t)(uintptr_t)MAP_FAILED);
        TEST_ASSERT("neither file nor anonymous is refused",
                    no_backing == (int64_t)(uintptr_t)MAP_FAILED);
        TEST_ASSERT("anonymous mapping succeeds", anon != (int64_t)(uintptr_t)MAP_FAILED);
    }
    TEST_PASS("mmap always has backing");

    /*
     * Address space released by one mapping must be available to the next. A
     * bump pointer never reclaims it, so a process that maps and unmaps walks
     * its address space away even though nothing is using it.
     */
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
    TEST_PASS("released address space is reusable");

    TEST_SUITE_END("Process");
}
