/*
 * test_mailbox.c - A firmware call that gets no answer gives up instead of spinning forever.
 */

#include "test.h"

#include <stdint.h>

#include "core/timer.h"
#include "driver/mailbox.h"

void test_mailbox(void)
{
    TEST_SUITE_BEGIN("Mailbox");

    uint32_t first[2] = {MBOX_CLOCK_ARM, 0};
    TEST_ASSERT("the firmware answers a query", mbox_query(MBOX_TAG_MAX_CLOCK_RATE, first) == 0);

    mbox_test_drop_next = 1;
    uint32_t lost[2] = {MBOX_CLOCK_ARM, 0};
    unsigned long start = timer_get_system_time();
    int rc = mbox_query(MBOX_TAG_MAX_CLOCK_RATE, lost);
    unsigned long took = timer_get_system_time() - start;
    mbox_test_drop_next = 0;
    TEST_ASSERT("an unanswered query fails", rc != 0);
    TEST_ASSERT("an unanswered query gives up within its bound", took < 2 * MBOX_TIMEOUT_MS);

    uint32_t again[2] = {MBOX_CLOCK_ARM, 0};
    TEST_ASSERT("the next query is answered",
                mbox_query(MBOX_TAG_MAX_CLOCK_RATE, again) == 0 && again[1] == first[1]);

    TEST_SUITE_END("Mailbox");
}
