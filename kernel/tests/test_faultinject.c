/*
 * test_faultinject.c - The out-of-memory paths, reached on purpose.
 *
 * A branch that handles a refused allocation never runs on a healthy machine,
 * so a leak or a half-built object on one goes unseen. Arming a one-shot
 * refusal walks them.
 */

#include <stddef.h>
#include <stdint.h>

#include "test.h"

#include "string.h"

#include "uapi/errno.h"
#include "uapi/fcntl.h"
#include "uapi/syscalls.h"

#include "arch/exception.h"

#include "core/syscall.h"
#include "mm/addr.h"
#include "mm/heap.h"
#include "mm/mmu.h"
#include "mm/pmm.h"
#include "sched/process.h"

// Where the path handed to the syscall probe below lives.
#define OOM_USER_VA 0x0000000051000000UL

static struct exception_trap_frame oom_tf;

static int64_t call_syscall(uint64_t nr, const uint64_t *args)
{
    memset(&oom_tf, 0, sizeof(oom_tf));
    oom_tf.x[8] = nr;
    for (int i = 0; i < 6; i++) {
        oom_tf.x[i] = args[i];
    }
    syscall_handle(&oom_tf);
    return (int64_t)oom_tf.x[0];
}

void test_faultinject(void)
{
    TEST_SUITE_BEGIN("Fault Injection");

    // the arming fires once, on the allocation it names, and not before
    {
        heap_test_fail_nth(2);

        void *first = heap_malloc(64);
        void *second = heap_malloc(64);
        void *third = heap_malloc(64);

        TEST_ASSERT("the allocation before the armed one succeeds", first != NULL);
        TEST_ASSERT("the armed allocation is refused", second == NULL);
        TEST_ASSERT("the allocation after it succeeds again", third != NULL);

        heap_free(first);
        heap_free(third);
        heap_test_fail_nth(0);
    }

    // a size the heap would refuse anyway must not consume the arming
    {
        heap_test_fail_nth(1);

        TEST_ASSERT("an impossible size is still refused", heap_malloc(0) == NULL);

        void *p = heap_malloc(64);
        TEST_ASSERT("the arming was spent on the real request", p == NULL);

        heap_test_fail_nth(0);
        void *q = heap_malloc(64);
        TEST_ASSERT("disarming restores the heap", q != NULL);
        heap_free(q);
    }

    // a refused page allocation leaks nothing back to the pool
    {
        unsigned long before = pmm_get_free_pages();

        pmm_test_fail_nth(1);
        void *page = pmm_alloc_page();
        TEST_ASSERT("the armed page allocation is refused", page == NULL);

        pmm_test_fail_nth(0);
        TEST_ASSERT_EQ("a refused page allocation costs nothing", pmm_get_free_pages(), before);

        void *ok = pmm_alloc_page();
        TEST_ASSERT("the pool still serves after a refusal", ok != NULL);
        pmm_free_page(ok);

        TEST_ASSERT_EQ("the pool is whole again", pmm_get_free_pages(), before);
    }

    // an address space is many pages, and refusing one partway through has to
    // unwind the rest: the free count is the assertion
    {
        unsigned long before = pmm_get_free_pages();

        for (unsigned long nth = 1; nth <= 4; nth++) {
            pmm_test_fail_nth(nth);

            unsigned long *pgd = mmu_create_user_pgd();
            if (pgd) {
                mmu_user_map_page(pgd, OOM_USER_VA, 0, MMU_PAGE_USER_DATA);
                mmu_destroy_user_pgd(pgd);
            }

            pmm_test_fail_nth(0);
        }

        TEST_ASSERT_EQ("a refused address space leaves no pages behind", pmm_get_free_pages(),
                       before);
    }

    // every user path is copied into the kernel first, and refusing that copy
    // must surface as -ENOMEM rather than a fault or a silent success
    {
        unsigned long *pgd = mmu_create_user_pgd();
        void *page = pgd ? pmm_alloc_page() : NULL;

        TEST_ASSERT("probe address space built", pgd != NULL && page != NULL);

        if (pgd && page) {
            mmu_user_map_page(pgd, OOM_USER_VA, V2P(page), MMU_PAGE_USER_DATA);
            strcpy((char *)page, "/bin/init.elf");

            unsigned long flags = spin_lock_irqsave(&process_table_lock);
            process_table[0]->user_pgd = pgd;
            spin_unlock_irqrestore(&process_table_lock, flags);

            uint64_t args[6] = {OOM_USER_VA, O_RDONLY, 0, 0, 0, 0};

            heap_test_fail_nth(1);
            int64_t ret = call_syscall(SYS_OPEN, args);
            heap_test_fail_nth(0);

            TEST_ASSERT_EQ("open reports the refused allocation", ret, -ENOMEM);

            flags = spin_lock_irqsave(&process_table_lock);
            process_table[0]->user_pgd = NULL;
            spin_unlock_irqrestore(&process_table_lock, flags);

            mmu_destroy_user_pgd(pgd);
        }
    }

    // nothing above may leave an arming behind for the suites that follow
    {
        void *p = heap_malloc(64);
        void *page = pmm_alloc_page();

        TEST_ASSERT("the heap is disarmed", p != NULL);
        TEST_ASSERT("the page allocator is disarmed", page != NULL);

        heap_free(p);
        pmm_free_page(page);
    }

    TEST_SUITE_END("Fault Injection");
}
