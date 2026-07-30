/*
 * test_signals.c - Post-init tests for signal delivery bookkeeping.
 *
 * Unlike the boot-phase suites these need a live process to target, so they
 * run after init has been created and address it by PID. Only the kernel-side
 * bookkeeping is checked here; running a handler requires returning to user
 * mode, which the test task never does.
 */

#include "test.h"

#include "string.h"

#include "uapi/errors.h"
#include "uapi/syscalls.h"

#include "arch/exception.h"

#include "core/signals.h"
#include "core/syscall.h"
#include "sched/process.h"

/* init is created first and is the process these tests target. */
#define INIT_PID 1

/* Driving SYS_KILL needs a trap frame; it is 800 bytes, so keep it off the stack. */
static struct exception_trap_frame kill_tf;

static int64_t call_kill(int64_t target_pid, int sig)
{
    memset(&kill_tf, 0, sizeof(kill_tf));
    kill_tf.x[8] = SYS_KILL;
    kill_tf.x[0] = (uint64_t)target_pid;
    kill_tf.x[1] = (uint64_t)sig;
    syscall_handle(&kill_tf);
    return (int64_t)kill_tf.x[0];
}

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
     * A zombie's task is freed as soon as it stops running, but its slot lives
     * on until a parent reaps it. Signalling one must be refused before
     * main_task is read: SIGKILL and SIGCONT dereference it first thing. The
     * poison pointer faults if that check is ever lost.
     */
    {
        int slot = process_test_claim_slot();
        int sent = 0;

        if (slot > 0) {
            unsigned long flags = spin_lock_irqsave(&process_table_lock);
            process_table[slot]->state = PROCESS_STATE_ZOMBIE;
            process_table[slot]->main_task = (struct task *)0xDEAD000000000000ULL;
            spin_unlock_irqrestore(&process_table_lock, flags);

            sent = signal_send((uint32_t)slot, SIGNAL_KILL);

            process_test_release_slot((uint32_t)slot);
        }

        TEST_ASSERT("zombie test slot claimed", slot > 0);
        TEST_ASSERT("zombie slot rejected", sent < 0);
    }
    TEST_PASS("zombie slot");

    /*
     * kill() takes a signed pid straight from the user. Everything outside
     * 1..PROCESS_TABLE_SIZE-1 must be refused before it indexes the table:
     * -1 used to clear the upper-bound check and read process_table[-1]->
     */
    {
        TEST_ASSERT_EQ("kill(-1) refused", call_kill(-1, SIGNAL_TERM), -PERS_ERR_NO_SUCH_PROCESS);
        TEST_ASSERT_EQ("kill(-1000) refused", call_kill(-1000, SIGNAL_TERM),
                       -PERS_ERR_NO_SUCH_PROCESS);
        TEST_ASSERT_EQ("kill(INT_MIN) refused", call_kill(-2147483647 - 1, SIGNAL_TERM),
                       -PERS_ERR_NO_SUCH_PROCESS);
        TEST_ASSERT_EQ("kill past the table refused", call_kill(PROCESS_TABLE_SIZE, SIGNAL_TERM),
                       -PERS_ERR_NO_SUCH_PROCESS);
        TEST_ASSERT_EQ("kill on an empty slot refused",
                       call_kill(PROCESS_TABLE_SIZE - 1, SIGNAL_TERM), -PERS_ERR_NO_SUCH_PROCESS);

        // pid 0 is the kernel and stays a permission error, not a lookup failure
        TEST_ASSERT_EQ("kill(0) refused", call_kill(0, SIGNAL_TERM), -PERS_ERR_PERMISSION_DENIED);
    }
    TEST_PASS("kill pid bounds");

    /*
     * A valid signal to a live process must be accepted and recorded in the
     * pending mask. The bit is (sig - 1) because signal numbering starts at 1.
     */
    {
        TEST_ASSERT_EQ("send SIGUSR1 to init", signal_send(INIT_PID, SIGNAL_USR1), 0);

        uint32_t pending = process_table[INIT_PID]->pending_signals;
        TEST_ASSERT("SIGUSR1 recorded as pending", (pending & (1u << (SIGNAL_USR1 - 1))) != 0);
    }
    TEST_PASS("pending bit set");

    // a second distinct signal must accumulate rather than replace
    {
        TEST_ASSERT_EQ("send SIGUSR2 to init", signal_send(INIT_PID, SIGNAL_USR2), 0);

        uint32_t pending = process_table[INIT_PID]->pending_signals;
        TEST_ASSERT("SIGUSR2 recorded", (pending & (1u << (SIGNAL_USR2 - 1))) != 0);
        TEST_ASSERT("SIGUSR1 still pending", (pending & (1u << (SIGNAL_USR1 - 1))) != 0);
    }
    TEST_PASS("pending signals accumulate");

    // re-sending an already-pending signal is idempotent, not a counter
    {
        uint32_t before = process_table[INIT_PID]->pending_signals;
        TEST_ASSERT_EQ("resend SIGUSR1", signal_send(INIT_PID, SIGNAL_USR1), 0);
        uint32_t after = process_table[INIT_PID]->pending_signals;
        TEST_ASSERT("resend leaves mask unchanged", before == after);
    }
    TEST_PASS("delivery is idempotent");

    // POSIX mutual discard: stop signals clear pending SIGCONT, and SIGCONT clears pending stop
    // signals
    {
        TEST_ASSERT_EQ("send SIGCONT", signal_send(INIT_PID, SIGNAL_CONT), 0);
        uint32_t pending = process_table[INIT_PID]->pending_signals;
        TEST_ASSERT("SIGCONT pending", (pending & (1u << (SIGNAL_CONT - 1))) != 0);

        TEST_ASSERT_EQ("send SIGSTOP", signal_send(INIT_PID, SIGNAL_STOP), 0);
        pending = process_table[INIT_PID]->pending_signals;
        TEST_ASSERT("SIGSTOP pending", (pending & (1u << (SIGNAL_STOP - 1))) != 0);
        TEST_ASSERT("SIGCONT cleared by SIGSTOP", (pending & (1u << (SIGNAL_CONT - 1))) == 0);

        TEST_ASSERT_EQ("send SIGCONT again", signal_send(INIT_PID, SIGNAL_CONT), 0);
        pending = process_table[INIT_PID]->pending_signals;
        TEST_ASSERT("SIGCONT pending again", (pending & (1u << (SIGNAL_CONT - 1))) != 0);
        TEST_ASSERT("SIGSTOP cleared by SIGCONT", (pending & (1u << (SIGNAL_STOP - 1))) == 0);

        /* Cleanup */
        process_table[INIT_PID]->pending_signals &=
            ~((1u << (SIGNAL_CONT - 1)) | (1u << (SIGNAL_STOP - 1)));
    }
    TEST_PASS("POSIX mutual signal discard");

    /*
     * Clear what this suite queued so init is not left holding signals it
     * never asked for once it runs.
     */
    process_table[INIT_PID]->pending_signals &=
        ~((1u << (SIGNAL_USR1 - 1)) | (1u << (SIGNAL_USR2 - 1)));
    TEST_ASSERT_EQ("test signals cleared",
                   (long)(process_table[INIT_PID]->pending_signals
                          & ((1u << (SIGNAL_USR1 - 1)) | (1u << (SIGNAL_USR2 - 1)))),
                   0);
    TEST_PASS("cleanup");

    TEST_SUITE_END("Signals");
}
