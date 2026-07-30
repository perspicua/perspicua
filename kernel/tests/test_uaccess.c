/*
 * test_uaccess.c - Tests for user-pointer validation and the fault-fixup path.
 *
 * These run in the boot phase with the kernel address space active, so they
 * cover the rejection and fault-recovery paths rather than a successful copy.
 * The point is that a bad user pointer must return an error: if the fixup path
 * were broken these would fault inside the kernel instead of failing cleanly.
 */

#include "test.h"

#include "string.h"

#include "uapi/errors.h"

#include "arch/uaccess.h"
#include "core/timer.h"
#include "mm/addr.h"
#include "mm/mmu.h"
#include "mm/pmm.h"

/* A canonical user-space address that is deliberately not mapped. */
#define UNMAPPED_USER_VA 0x0000000040000000UL

/* A separate VA for the page the string tests read from. */
#define MAPPED_USER_VA 0x0000000050000000UL

void test_uaccess(void)
{
    TEST_SUITE_BEGIN("User Access");

    static char kbuf[64];

    // degenerate arguments are rejected rather than dereferenced
    {
        TEST_ASSERT_EQ("NULL pointer rejected", validate_user_buffer(NULL, 8, 0), 0);
        TEST_ASSERT_EQ("zero length rejected", validate_user_buffer((void *)UNMAPPED_USER_VA, 0, 0),
                       0);
    }
    TEST_PASS("degenerate arguments");

    // a kernel address must never pass as a user buffer
    {
        TEST_ASSERT_EQ("kernel address rejected", validate_user_buffer(kbuf, sizeof(kbuf), 0), 0);
        TEST_ASSERT_EQ("kernel address rejected for write",
                       validate_user_buffer(kbuf, sizeof(kbuf), 1), 0);
        TEST_ASSERT_EQ("KERNEL_VMA boundary rejected",
                       validate_user_buffer((void *)KERNEL_VMA, 8, 0), 0);
    }
    TEST_PASS("kernel addresses");

    /*
     * A length that wraps past the top of the address space must be caught by
     * the explicit end < start check, not silently truncated into a range that
     * looks valid.
     */
    {
        TEST_ASSERT_EQ("wrap-around rejected",
                       validate_user_buffer((void *)0xFFFFFFFFFFFFFFF0UL, 64, 0), 0);
        TEST_ASSERT_EQ("range crossing into kernel rejected",
                       validate_user_buffer((void *)(KERNEL_VMA - 8), 64, 0), 0);
    }
    TEST_PASS("range overflow");

    // an unmapped user address is well-formed but has no translation
    {
        TEST_ASSERT_EQ("unmapped user VA rejected",
                       validate_user_buffer((void *)UNMAPPED_USER_VA, 64, 0), 0);
    }
    TEST_PASS("unmapped user address");

    /*
     * The copy helpers must survive a bad pointer via the exception fixup
     * table. Reaching the assertion at all proves the fault was recovered
     * rather than escalating to a kernel panic.
     */
    {
        memset(kbuf, 0xA5, sizeof(kbuf));

        int res = copy_from_user(kbuf, (const void *)UNMAPPED_USER_VA, 32);
        TEST_ASSERT("copy_from_user on unmapped VA fails", res != 0);
        TEST_ASSERT("copy_from_user left destination untouched", (unsigned char)kbuf[0] == 0xA5);

        res = copy_to_user((void *)UNMAPPED_USER_VA, kbuf, 32);
        TEST_ASSERT("copy_to_user to unmapped VA fails", res != 0);
    }
    TEST_PASS("fault fixup recovers");

    // strncpy_from_user must fail on a bad source without writing a string
    {
        memset(kbuf, 0xA5, sizeof(kbuf));
        long res = strncpy_from_user(kbuf, (const char *)UNMAPPED_USER_VA, sizeof(kbuf));
        TEST_ASSERT("strncpy_from_user on unmapped VA fails", res < 0);
        TEST_ASSERT("strncpy_from_user terminates on fault", kbuf[0] == '\0');
    }
    TEST_PASS("strncpy_from_user fixup");

    /*
     * The truncation contract needs a source EL0 can actually read, so map one
     * page into a scratch address space and run TTBR0 on it. A caller must never
     * be handed a buffer without a terminator: strlen() on one walks off the
     * allocation.
     */
    {
        unsigned long *pgd = mmu_create_user_pgd();
        void *page = pmm_alloc_page();
        TEST_ASSERT("scratch pgd and page allocated", pgd != NULL && page != NULL);

        mmu_user_map_page(pgd, MAPPED_USER_VA, V2P(page), MMU_PAGE_USER_DATA);
        strcpy((char *)page, "abcdefghij"); // 10 chars

        /* schedule() reinstalls the running task's TTBR0, so a timer tick here
         * would swap the scratch space out mid-test. Keep IRQs masked. */
        unsigned long irqf = irq_save();

        unsigned long saved_ttbr0;
        asm volatile("mrs %0, ttbr0_el1" : "=r"(saved_ttbr0));
        mmu_switch_user(pgd, 0);

        const char *usrc = (const char *)MAPPED_USER_VA;

        memset(kbuf, 0xA5, sizeof(kbuf));
        long res = strncpy_from_user(kbuf, usrc, sizeof(kbuf));
        TEST_ASSERT_EQ("full copy returns the length", res, 10);
        TEST_ASSERT("full copy is terminated", strcmp(kbuf, "abcdefghij") == 0);

        // exactly enough room for the string and its terminator
        memset(kbuf, 0xA5, sizeof(kbuf));
        res = strncpy_from_user(kbuf, usrc, 11);
        TEST_ASSERT_EQ("exact fit returns the length", res, 10);
        TEST_ASSERT("exact fit is terminated", strcmp(kbuf, "abcdefghij") == 0);

        // one byte short: must report truncation, never a bare prefix
        memset(kbuf, 0xA5, sizeof(kbuf));
        res = strncpy_from_user(kbuf, usrc, 10);
        TEST_ASSERT_EQ("truncation reported", res, -PERS_ERR_NAME_TOO_LONG);
        TEST_ASSERT("truncated buffer is terminated", kbuf[9] == '\0');
        TEST_ASSERT("truncated buffer holds the prefix", strcmp(kbuf, "abcdefghi") == 0);

        // a single byte can only hold a terminator
        memset(kbuf, 0xA5, sizeof(kbuf));
        res = strncpy_from_user(kbuf, usrc, 1);
        TEST_ASSERT_EQ("count 1 reports truncation", res, -PERS_ERR_NAME_TOO_LONG);
        TEST_ASSERT("count 1 still terminates", kbuf[0] == '\0');

        // an empty source fits in one byte
        ((char *)page)[0] = '\0';
        memset(kbuf, 0xA5, sizeof(kbuf));
        res = strncpy_from_user(kbuf, usrc, 1);
        TEST_ASSERT_EQ("empty string in one byte returns 0", res, 0);
        TEST_ASSERT("empty string is terminated", kbuf[0] == '\0');

        // zero and negative counts have nowhere to put a terminator
        TEST_ASSERT_EQ("count 0 rejected", strncpy_from_user(kbuf, usrc, 0),
                       -PERS_ERR_NAME_TOO_LONG);
        TEST_ASSERT_EQ("negative count rejected", strncpy_from_user(kbuf, usrc, -1),
                       -PERS_ERR_NAME_TOO_LONG);

        asm volatile("msr ttbr0_el1, %0" ::"r"(saved_ttbr0) : "memory");
        asm volatile("dsb ish\n tlbi vmalle1is\n dsb ish\n isb" ::: "memory");
        irq_restore(irqf);

        /* Frees the mapped page along with the tables. */
        mmu_destroy_user_pgd(pgd);
    }
    TEST_PASS("strncpy_from_user always terminates");

    TEST_SUITE_END("User Access");
}
