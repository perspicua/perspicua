/*
 * test_tty.c - Console input control characters, RX ring overflow and an interrupted write.
 */

#include <stddef.h>
#include <stdint.h>

#include "test.h"

#include "string.h"

#include "core/signals.h"
#include "core/tty.h"
#include "driver/uart.h"
#include "sched/process.h"

static volatile int writer_done;
static volatile int writer_ret;
static struct task *writer_task;
static char writer_buf[2 * TTY_BUFFER_SIZE];

static void task_tty_writer(void)
{
    writer_task = sched_current_task();
    writer_ret = tty_write(&console_tty, writer_buf, sizeof(writer_buf));
    writer_done = 1;
}

static size_t rx_pending(void)
{
    unsigned long flags = spin_lock_irqsave(&console_tty.lock);
    size_t n = (console_tty.rx_head + TTY_BUFFER_SIZE - console_tty.rx_tail) % TTY_BUFFER_SIZE;
    spin_unlock_irqrestore(&console_tty.lock, flags);
    return n;
}

static void rx_discard(void)
{
    unsigned long flags = spin_lock_irqsave(&console_tty.lock);
    console_tty.rx_tail = console_tty.rx_head;
    spin_unlock_irqrestore(&console_tty.lock, flags);
}

// Empties the TX ring without sending it.
static void tx_discard(void)
{
    unsigned long flags = spin_lock_irqsave(&console_tty.lock);
    unsigned long uart_flags = spin_lock_irqsave(&uart_tx_lock);
    console_tty.tx_tail = console_tty.tx_head;
    spin_unlock_irqrestore(&uart_tx_lock, uart_flags);
    spin_unlock_irqrestore(&console_tty.lock, flags);
}

static uint32_t set_foreground(uint32_t pgid)
{
    unsigned long flags = spin_lock_irqsave(&console_tty.lock);
    uint32_t old = console_tty.foreground_pgid;
    console_tty.foreground_pgid = pgid;
    spin_unlock_irqrestore(&console_tty.lock, flags);
    return old;
}

void test_tty_scheduler(void)
{
    TEST_SUITE_BEGIN("TTY");

    // a byte that finds the RX ring full is counted, not lost without a trace
    {
        rx_discard();
        unsigned long dropped = console_tty.rx_dropped;
        for (int i = 0; i < TTY_BUFFER_SIZE + 9; i++) {
            tty_handle_rx(&console_tty, 'x');
        }
        TEST_ASSERT_EQ("rx: the ring keeps all but one slot", rx_pending(), TTY_BUFFER_SIZE - 1);
        TEST_ASSERT_EQ("rx: each byte past it is counted", console_tty.rx_dropped - dropped, 10);
        rx_discard();
    }

    // ^C discards what was typed before it
    {
        uint32_t saved = set_foreground(0);
        tty_handle_rx(&console_tty, 'a');
        tty_handle_rx(&console_tty, 'b');
        tty_handle_rx(&console_tty, 3);
        set_foreground(saved);
        TEST_ASSERT_EQ("^C: typed-ahead input is discarded", rx_pending(), 0);
    }

    // ^\ sends SIGQUIT to the foreground group
    {
        int slot = process_test_claim_slot();
        TEST_ASSERT("^\\: test slot claimed", slot > 0);

        if (slot > 0) {
            struct process *p = process_table[slot];
            unsigned long flags = spin_lock_irqsave(&process_table_lock);
            p->pgid = (uint32_t)slot;
            spin_unlock_irqrestore(&process_table_lock, flags);

            uint32_t saved = set_foreground((uint32_t)slot);
            tty_handle_rx(&console_tty, 28);
            set_foreground(saved);

            TEST_ASSERT("^\\: the foreground group gets SIGQUIT",
                        (p->pending_signals & (1u << (SIGQUIT - 1))) != 0);
            TEST_ASSERT_EQ("^\\: is not itself input", rx_pending(), 0);

            process_test_release_slot((uint32_t)slot);
        }
    }

    // a writer waiting for ring space is let go by a signal and reports what it queued
    {
        struct process *kproc = process_table[0];
        memset(writer_buf, ' ', sizeof(writer_buf));
        writer_done = 0;
        writer_ret = 0;
        writer_task = NULL;

        tty_test_stall_tx(1);
        sched_create_task(task_tty_writer);
        WAIT_UNTIL(task_blocked(writer_task) && console_tty.tx_wq.head != NULL);
        TEST_ASSERT("write: the writer waits for ring space",
                    !writer_done && task_blocked(writer_task));

        __atomic_fetch_or(&kproc->pending_signals, 1u << (SIGUSR1 - 1), __ATOMIC_SEQ_CST);
        if (writer_task) {
            sched_unblock(writer_task);
        }
        WAIT_UNTIL(writer_done);
        __atomic_fetch_and(&kproc->pending_signals, ~(1u << (SIGUSR1 - 1)), __ATOMIC_SEQ_CST);

        TEST_ASSERT("write: a signal ends the wait", writer_done);
        TEST_ASSERT_EQ("write: what was queued counts as written", writer_ret, TTY_BUFFER_SIZE - 1);

        // Spaces, not sent: a writer the signal did not release fills the ring again and is drained.
        tx_discard();
        tty_test_stall_tx(0);
        while (!writer_done) {
            tx_discard();
            wq_wake_all(&console_tty.tx_wq);
            sched_sleep_ms(5);
        }
        tx_discard();
    }

    TEST_SUITE_END("TTY");
}
