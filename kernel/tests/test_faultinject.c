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

#include "fs/vfs.h"
#include "mm/heap.h"
#include "mm/mmu.h"
#include "mm/pmm.h"

// Where the path handed to the syscall probe below lives.
#define OOM_USER_VA 0x0000000051000000UL
#define OOM_PATH    "/bin/init.elf"

// Past the last allocation open makes, so the tail of the walk succeeds.
#define OOM_OPEN_MAX_NTH 16

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

    // each allocation open makes, refused in turn, surfaces as -ENOMEM and
    // leaves no descriptor behind
    {
        unsigned long *pgd = test_borrow_user_pgd();
        char *path = pgd ? test_map_user_page(pgd, OOM_USER_VA, MMU_PAGE_USER_DATA) : NULL;

        TEST_ASSERT("probe address space built", path != NULL);

        if (path) {
            strcpy(path, OOM_PATH);
            uint64_t args[6] = {OOM_USER_VA, O_RDONLY, 0, 0, 0, 0};

            int64_t first_fd = test_syscall(SYS_OPEN, args);
            TEST_ASSERT("open succeeds unarmed", first_fd >= 0);
            if (first_fd >= 0) {
                vfs_close((int)first_fd);
            }

            int enomem_ok = 1;
            int refusals = 0;
            int walked_past = 0;

            for (unsigned long nth = 1; nth <= OOM_OPEN_MAX_NTH; nth++) {
                heap_test_fail_nth(nth);
                int64_t ret = test_syscall(SYS_OPEN, args);
                heap_test_fail_nth(0);

                if (ret >= 0) {
                    vfs_close((int)ret);
                    walked_past = 1;
                } else if (ret == -ENOMEM) {
                    refusals++;
                } else {
                    pr_err("test: open with allocation %lu refused returned %ld [FAILED]\n", nth,
                           (long)ret);
                    enomem_ok = 0;
                }
            }

            TEST_ASSERT("open's allocations can be refused", refusals > 0);
            TEST_ASSERT("the walk reaches past open's last allocation", walked_past);
            TEST_ASSERT("every refusal is reported as -ENOMEM", enomem_ok);

            int64_t last_fd = test_syscall(SYS_OPEN, args);
            TEST_ASSERT_EQ("a refused open leaves no descriptor behind", last_fd, first_fd);
            if (last_fd >= 0) {
                vfs_close((int)last_fd);
            }
        }

        if (pgd) {
            test_release_user_pgd(pgd);
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
