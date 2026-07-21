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

#include "arch/uaccess.h"
#include "mm/addr.h"

/* A canonical user-space address that is deliberately not mapped. */
#define UNMAPPED_USER_VA 0x0000000040000000UL

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
    }
    TEST_PASS("strncpy_from_user fixup");

    TEST_SUITE_END("User Access");
}
