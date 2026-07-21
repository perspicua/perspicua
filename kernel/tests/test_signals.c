/*
 * test_signals.c - Post-init tests for signal delivery bookkeeping.
 *
 * Unlike the boot-phase suites these need a live process to target, so they
 * run after init has been created and address it by PID. Only the kernel-side
 * bookkeeping is checked here; running a handler requires returning to user
 * mode, which the test task never does.
 */

#include "test.h"

#include "core/signals.h"
#include "sched/process.h"
#include "uapi/errors.h"

/* init is created first and is the process these tests target. */
#define INIT_PID 1

void test_signals(void)
{
    TEST_SUITE_BEGIN("Signals");

    // signal numbers outside 1..SIGNAL_COUNT-1 are rejected
    {
        TEST_ASSERT("signal 0 rejected", signal_send(INIT_PID, 0) < 0);
        TEST_ASSERT("negative signal rejected", signal_send(INIT_PID, -1) < 0);
        TEST_ASSERT("signal SIGNAL_COUNT rejected", signal_send(INIT_PID, SIGNAL_COUNT) < 0);
        TEST_ASSERT("far out-of-range signal rejected", signal_send(INIT_PID, 9999) < 0);
    }
    TEST_PASS("signal number validation");

    // pid 0 and out-of-table pids have no process to receive anything
    {
        TEST_ASSERT("pid 0 rejected", signal_send(0, SIGNAL_USR1) < 0);
        TEST_ASSERT("out-of-range pid rejected", signal_send(0xFFFFFFFFu, SIGNAL_USR1) < 0);
    }
    TEST_PASS("target validation");

    // an empty process slot is not a valid target
    {
        int sent = signal_send(PROCESS_TABLE_SIZE - 1, SIGNAL_USR1);
        TEST_ASSERT("empty slot rejected", sent < 0);
    }
    TEST_PASS("empty slot");

    /*
     * A valid signal to a live process must be accepted and recorded in the
     * pending mask. The bit is (sig - 1) because signal numbering starts at 1.
     */
    {
        TEST_ASSERT_EQ("send SIGUSR1 to init", signal_send(INIT_PID, SIGNAL_USR1), 0);

        uint32_t pending = process_table[INIT_PID].pending_signals;
        TEST_ASSERT("SIGUSR1 recorded as pending", (pending & (1u << (SIGNAL_USR1 - 1))) != 0);
    }
    TEST_PASS("pending bit set");

    // a second distinct signal must accumulate rather than replace
    {
        TEST_ASSERT_EQ("send SIGUSR2 to init", signal_send(INIT_PID, SIGNAL_USR2), 0);

        uint32_t pending = process_table[INIT_PID].pending_signals;
        TEST_ASSERT("SIGUSR2 recorded", (pending & (1u << (SIGNAL_USR2 - 1))) != 0);
        TEST_ASSERT("SIGUSR1 still pending", (pending & (1u << (SIGNAL_USR1 - 1))) != 0);
    }
    TEST_PASS("pending signals accumulate");

    // re-sending an already-pending signal is idempotent, not a counter
    {
        uint32_t before = process_table[INIT_PID].pending_signals;
        TEST_ASSERT_EQ("resend SIGUSR1", signal_send(INIT_PID, SIGNAL_USR1), 0);
        uint32_t after = process_table[INIT_PID].pending_signals;
        TEST_ASSERT("resend leaves mask unchanged", before == after);
    }
    TEST_PASS("delivery is idempotent");

    /*
     * Clear what this suite queued so init is not left holding signals it
     * never asked for once it runs.
     */
    process_table[INIT_PID].pending_signals &=
        ~((1u << (SIGNAL_USR1 - 1)) | (1u << (SIGNAL_USR2 - 1)));
    TEST_ASSERT_EQ("test signals cleared",
                   (long)(process_table[INIT_PID].pending_signals
                          & ((1u << (SIGNAL_USR1 - 1)) | (1u << (SIGNAL_USR2 - 1)))),
                   0);
    TEST_PASS("cleanup");

    TEST_SUITE_END("Signals");
}
