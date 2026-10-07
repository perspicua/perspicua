/*
 * test_wait.c - Tests for the generic wait queue implementation.
 */

#include "sched/wait.h"

#include <stddef.h>

#include "arch/irq.h"
#include "core/lock.h"
#include "core/mutex.h"
#include "core/signals.h"
#include "core/timer.h"
#include "core/tty.h"
#include "driver/block.h"
#include "driver/sd.h"
#include "fs/pipe.h"
#include "fs/vfs.h"
#include "sched/process.h"
#include "sched/sched.h"
#include "string.h"
#include "test.h"
#include "uapi/errno.h"

// Simulates a scheduler transition; a failed CAS reports instead of overwriting a READY.
static void test_move_state(struct task *t, enum sched_task_state from, enum sched_task_state to)
{
    enum sched_task_state expected = from;
    TEST_ASSERT("simulated state change starts from the expected state",
                __atomic_compare_exchange_n(&t->state, &expected, to, 0, __ATOMIC_SEQ_CST,
                                            __ATOMIC_SEQ_CST));
}

void test_wait(void)
{
    TEST_SUITE_BEGIN("WaitQueue");

    struct task *self = sched_current_task();

    // A wake that raced an earlier wait can leave this task READY and queued; let that copy run.
    sched_schedule();
    TEST_ASSERT_EQ("test task starts RUNNING", (long)self->state, (long)SCHED_TASK_RUNNING);

    // 1. Initialisation and static initialiser match
    {
        struct wait_queue wq;
        wq_init(&wq);
        TEST_ASSERT("init lock unlocked", wq.lock.locked == 0);
        TEST_ASSERT("init head null", wq.head == NULL);
        TEST_ASSERT("init tail null", wq.tail == NULL);

        struct wait_queue statically = WAIT_QUEUE_INIT;
        TEST_ASSERT("static lock unlocked", statically.lock.locked == 0);
        TEST_ASSERT("static head null", statically.head == NULL);
        TEST_ASSERT("static tail null", statically.tail == NULL);
    }

    // 2. Prepare and finish a single waiter
    {
        struct wait_queue wq = WAIT_QUEUE_INIT;
        struct wait_entry e = {0};

        unsigned long flags = irq_save();
        wq_prepare(&wq, &e);
        TEST_ASSERT("prepare links head", wq.head == &e);
        TEST_ASSERT("prepare links tail", wq.tail == &e);
        TEST_ASSERT("entry queue recorded", e.queue == &wq);
        TEST_ASSERT("entry next null", e.next == NULL);
        TEST_ASSERT_EQ("entry woken 0", (long)e.woken, 0);
        TEST_ASSERT("entry task is current", e.task == self);
        TEST_ASSERT_EQ("prepare sets BLOCKED state", (long)self->state, (long)SCHED_TASK_BLOCKED);

        // Spurious wakeup: re-prepare keeps place in line
        wq_prepare(&wq, &e);
        TEST_ASSERT("spurious prepare keeps head", wq.head == &e);
        TEST_ASSERT("spurious prepare keeps tail", wq.tail == &e);
        TEST_ASSERT("spurious prepare keeps next null", e.next == NULL);

        wq_finish(&wq, &e);
        irq_restore(flags);

        TEST_ASSERT("finish unlinks head", wq.head == NULL);
        TEST_ASSERT("finish unlinks tail", wq.tail == NULL);
        TEST_ASSERT("finish clears entry queue", e.queue == NULL);
        TEST_ASSERT("finish clears entry next", e.next == NULL);
        TEST_ASSERT_EQ("finish restores RUNNING state", (long)self->state,
                       (long)SCHED_TASK_RUNNING);
    }

    // 3. FIFO ordering and wake_one
    {
        struct wait_queue wq = WAIT_QUEUE_INIT;
        struct wait_entry e1 = {0};
        struct wait_entry e2 = {0};
        struct wait_entry e3 = {0};

        unsigned long flags = irq_save();
        wq_prepare(&wq, &e1);
        e1.task = NULL;
        wq_prepare(&wq, &e2);
        e2.task = NULL;
        wq_prepare(&wq, &e3);
        e3.task = NULL;

        TEST_ASSERT("head is e1", wq.head == &e1);
        TEST_ASSERT("e1 next is e2", e1.next == &e2);
        TEST_ASSERT("e2 next is e3", e2.next == &e3);
        TEST_ASSERT("e3 next is null", e3.next == NULL);
        TEST_ASSERT("tail is e3", wq.tail == &e3);

        // First wake pops e1
        int w1 = wq_wake_one(&wq);
        TEST_ASSERT_EQ("wake_one returns 1", (long)w1, 1);
        TEST_ASSERT_EQ("e1 woken set", (long)e1.woken, 1);
        TEST_ASSERT("e1 queue cleared", e1.queue == NULL);
        TEST_ASSERT("e1 next cleared", e1.next == NULL);
        TEST_ASSERT("head advances to e2", wq.head == &e2);
        TEST_ASSERT("tail remains e3", wq.tail == &e3);

        // Second wake pops e2
        int w2 = wq_wake_one(&wq);
        TEST_ASSERT_EQ("wake_one returns 1", (long)w2, 1);
        TEST_ASSERT_EQ("e2 woken set", (long)e2.woken, 1);
        TEST_ASSERT("e2 queue cleared", e2.queue == NULL);
        TEST_ASSERT("head advances to e3", wq.head == &e3);
        TEST_ASSERT("tail remains e3", wq.tail == &e3);

        // Third wake pops e3 (last entry)
        int w3 = wq_wake_one(&wq);
        TEST_ASSERT_EQ("wake_one returns 1", (long)w3, 1);
        TEST_ASSERT_EQ("e3 woken set", (long)e3.woken, 1);
        TEST_ASSERT("e3 queue cleared", e3.queue == NULL);
        TEST_ASSERT("head is null", wq.head == NULL);
        TEST_ASSERT("tail is null", wq.tail == NULL);

        // Fourth wake on empty queue returns 0
        int w4 = wq_wake_one(&wq);
        TEST_ASSERT_EQ("wake on empty queue returns 0", (long)w4, 0);

        // Finish on already woken entries preserves woken flag
        wq_finish(&wq, &e1);
        wq_finish(&wq, &e2);
        wq_finish(&wq, &e3);
        irq_restore(flags);

        TEST_ASSERT_EQ("e1 woken preserved after finish", (long)e1.woken, 1);
        TEST_ASSERT_EQ("e2 woken preserved after finish", (long)e2.woken, 1);
        TEST_ASSERT_EQ("e3 woken preserved after finish", (long)e3.woken, 1);
    }

    // 4. Unlinking middle and tail entries via wq_finish
    {
        struct wait_queue wq = WAIT_QUEUE_INIT;
        struct wait_entry e1 = {0};
        struct wait_entry e2 = {0};
        struct wait_entry e3 = {0};

        unsigned long flags = irq_save();
        wq_prepare(&wq, &e1);
        e1.task = NULL;
        wq_prepare(&wq, &e2);
        e2.task = NULL;
        wq_prepare(&wq, &e3);
        e3.task = NULL;

        // Finish middle entry e2
        wq_finish(&wq, &e2);
        TEST_ASSERT("head still e1 after middle finish", wq.head == &e1);
        TEST_ASSERT("e1 next points to e3", e1.next == &e3);
        TEST_ASSERT("tail still e3 after middle finish", wq.tail == &e3);
        TEST_ASSERT("e2 queue cleared", e2.queue == NULL);
        TEST_ASSERT("e2 next cleared", e2.next == NULL);

        // Finish tail entry e3
        wq_finish(&wq, &e3);
        TEST_ASSERT("head still e1 after tail finish", wq.head == &e1);
        TEST_ASSERT("tail updated to e1", wq.tail == &e1);
        TEST_ASSERT("e1 next is null", e1.next == NULL);
        TEST_ASSERT("e3 queue cleared", e3.queue == NULL);

        // Finish remaining entry e1
        wq_finish(&wq, &e1);
        irq_restore(flags);

        TEST_ASSERT("queue empty after all finishes", wq.head == NULL && wq.tail == NULL);
    }

    // 5. wq_wake_all wakes every entry
    {
        struct wait_queue wq = WAIT_QUEUE_INIT;
        struct wait_entry e1 = {0};
        struct wait_entry e2 = {0};
        struct wait_entry e3 = {0};

        unsigned long flags = irq_save();
        wq_prepare(&wq, &e1);
        e1.task = NULL;
        wq_prepare(&wq, &e2);
        e2.task = NULL;
        wq_prepare(&wq, &e3);
        e3.task = NULL;

        wq_wake_all(&wq);
        test_move_state(self, SCHED_TASK_BLOCKED, SCHED_TASK_RUNNING);
        irq_restore(flags);

        TEST_ASSERT("wake_all empties head", wq.head == NULL);
        TEST_ASSERT("wake_all empties tail", wq.tail == NULL);
        TEST_ASSERT_EQ("wake_all woke e1", (long)e1.woken, 1);
        TEST_ASSERT_EQ("wake_all woke e2", (long)e2.woken, 1);
        TEST_ASSERT_EQ("wake_all woke e3", (long)e3.woken, 1);
        TEST_ASSERT("e1 queue cleared", e1.queue == NULL);
        TEST_ASSERT("e2 queue cleared", e2.queue == NULL);
        TEST_ASSERT("e3 queue cleared", e3.queue == NULL);
        TEST_ASSERT("e1 next cleared", e1.next == NULL);
        TEST_ASSERT("e2 next cleared", e2.next == NULL);
        TEST_ASSERT("e3 next cleared", e3.next == NULL);
    }

    // 6. wq_finish CAS state transitions
    {
        struct wait_queue wq = WAIT_QUEUE_INIT;
        struct wait_entry e = {0};

        // If state was set to READY by a waker, finish leaves it READY
        unsigned long flags = irq_save();
        wq_prepare(&wq, &e);
        test_move_state(self, SCHED_TASK_BLOCKED, SCHED_TASK_READY);
        wq_finish(&wq, &e);
        TEST_ASSERT_EQ("finish leaves READY state intact", (long)self->state,
                       (long)SCHED_TASK_READY);
        test_move_state(self, SCHED_TASK_READY, SCHED_TASK_RUNNING);

        // If state was set to RUNNING, finish leaves it RUNNING
        wq_prepare(&wq, &e);
        test_move_state(self, SCHED_TASK_BLOCKED, SCHED_TASK_RUNNING);
        wq_finish(&wq, &e);
        TEST_ASSERT_EQ("finish leaves RUNNING state intact", (long)self->state,
                       (long)SCHED_TASK_RUNNING);
        irq_restore(flags);
    }

    // 7. Wait macros with already-true conditions
    {
        struct wait_queue wq = WAIT_QUEUE_INIT;
        int cond = 1;

        int r1 = wq_wait_event(&wq, cond);
        TEST_ASSERT_EQ("wait_event returns 0 when cond true", (long)r1, 0);
        TEST_ASSERT("queue empty after wait_event", wq.head == NULL && wq.tail == NULL);
        TEST_ASSERT_EQ("state RUNNING after wait_event", (long)self->state,
                       (long)SCHED_TASK_RUNNING);

        int r2 = wq_wait_event_interruptible(&wq, cond);
        TEST_ASSERT_EQ("wait_event_interruptible returns 0 when cond true", (long)r2, 0);
        TEST_ASSERT("queue empty after interruptible", wq.head == NULL && wq.tail == NULL);
        TEST_ASSERT_EQ("state RUNNING after interruptible", (long)self->state,
                       (long)SCHED_TASK_RUNNING);

        spinlock_t lock = SPINLOCK_INIT;
        unsigned long flags = spin_lock_irqsave(&lock);
        int r3 = wq_wait_event_locked(&wq, cond, &lock);
        spin_unlock_irqrestore(&lock, flags);
        TEST_ASSERT_EQ("wait_event_locked returns 0 when cond true", (long)r3, 0);
        TEST_ASSERT("queue empty after wait_event_locked", wq.head == NULL && wq.tail == NULL);

        flags = spin_lock_irqsave(&lock);
        int r4 = wq_wait_event_interruptible_locked(&wq, cond, &lock);
        spin_unlock_irqrestore(&lock, flags);
        TEST_ASSERT_EQ("wait_event_interruptible_locked returns 0 when cond true", (long)r4, 0);
        TEST_ASSERT("queue empty after interruptible locked", wq.head == NULL && wq.tail == NULL);
    }

    TEST_SUITE_END("WaitQueue");
}

/*
 * Multi-task tests running after sched_init and enable_interrupts.
 */

static struct wait_queue sched_test_wq = WAIT_QUEUE_INIT;
static volatile int sched_test_cond = 0;
static volatile int sched_test_done = 0;
static volatile int sched_test_ret = 0;
static struct task *sched_test_task_ptr = NULL;

static void task_sleep_wait(void)
{
    sched_test_task_ptr = sched_current_task();
    wq_wait_event(&sched_test_wq, sched_test_cond != 0);
    sched_test_done = 1;
}

static void task_sleep_interruptible(void)
{
    sched_test_task_ptr = sched_current_task();
    sched_test_ret = wq_wait_event_interruptible(&sched_test_wq, sched_test_cond != 0);
    sched_test_done = 1;
}

static volatile int regression_done = 0;

static void task_regression_ready_overwrite(void)
{
    unsigned long flags = irq_save();
    struct task *self = sched_current_task();
    struct wait_queue wq = WAIT_QUEUE_INIT;
    struct wait_entry e = {0};

    // Race 1: prepare sets BLOCKED, unblock sets READY and enqueues on run queue.
    wq_prepare(&wq, &e);
    sched_unblock(self);
    TEST_ASSERT_EQ("repro: after first race state is READY", (long)self->state,
                   (long)SCHED_TASK_READY);
    TEST_ASSERT("repro: self in run queue", sched_test_in_run_queue(self));

    // Race 2: second prepare while still on run queue must NOT overwrite READY with BLOCKED.
    wq_prepare(&wq, &e);
    TEST_ASSERT_EQ("repro: second prepare keeps state READY", (long)self->state,
                   (long)SCHED_TASK_READY);

    sched_unblock(self);
    TEST_ASSERT("repro: self->rq_next != self", self->rq_next != self);
    TEST_ASSERT_EQ("repro: state still READY after second unblock", (long)self->state,
                   (long)SCHED_TASK_READY);

    wq_finish(&wq, &e);

    regression_done = 1;
    irq_restore(flags);
}

static struct wait_queue passon_wq = WAIT_QUEUE_INIT;
static volatile int passon_token = 0;
static volatile int passon_cond_armed = 0;
static volatile int passon_task_a_done = 0;
static volatile int passon_task_a_ret = 0;
static volatile int passon_task_b_done = 0;
static struct task *passon_task_a = NULL;

static int cond_func_a(void)
{
    if (passon_cond_armed == 1) {
        passon_cond_armed = 2;
        passon_token = 1;
        struct process *kproc = process_table[0];
        if (kproc) {
            __atomic_fetch_or(&kproc->pending_signals, 1u << (SIGUSR1 - 1), __ATOMIC_SEQ_CST);
        }
        wq_wake_one(&passon_wq);
    }
    return 0;
}

static void task_passon_a(void)
{
    passon_task_a = sched_current_task();
    passon_task_a_ret = wq_wait_event_interruptible(&passon_wq, cond_func_a());
    passon_task_a_done = 1;
}

static struct task *passon_task_b = NULL;

static void task_passon_b(void)
{
    passon_task_b = sched_current_task();
    wq_wait_event(&passon_wq, passon_token != 0);
    passon_task_b_done = 1;
}

static struct kmutex test_contend_mutex = KMUTEX_INIT;
static volatile int mutex_cs_count = 0;
static volatile int mutex_cs_violation = 0;
static volatile int holder_done = 0;
static volatile int holder_release = 0;
static volatile int contender1_done = 0;
static volatile int contender2_done = 0;
static volatile int contender_order[2] = {0, 0};
static volatile int contender_order_idx = 0;

static void task_contend_holder(void)
{
    kmutex_lock(&test_contend_mutex);
    mutex_cs_count++;
    if (mutex_cs_count > 1) {
        mutex_cs_violation = 1;
    }
    while (!holder_release) {
        sched_sleep_ms(5);
    }
    mutex_cs_count--;
    holder_done = 1;
    kmutex_unlock(&test_contend_mutex);
}

static struct task *contender1_task = NULL;
static struct task *contender2_task = NULL;

static void task_contender1(void)
{
    contender1_task = sched_current_task();
    kmutex_lock(&test_contend_mutex);
    mutex_cs_count++;
    if (mutex_cs_count > 1) {
        mutex_cs_violation = 1;
    }
    contender_order[contender_order_idx++] = 1;
    sched_sleep_ms(10);
    mutex_cs_count--;
    contender1_done = 1;
    kmutex_unlock(&test_contend_mutex);
}

static void task_contender2(void)
{
    contender2_task = sched_current_task();
    kmutex_lock(&test_contend_mutex);
    mutex_cs_count++;
    if (mutex_cs_count > 1) {
        mutex_cs_violation = 1;
    }
    contender_order[contender_order_idx++] = 2;
    sched_sleep_ms(10);
    mutex_cs_count--;
    contender2_done = 1;
    kmutex_unlock(&test_contend_mutex);
}

static volatile int sig_holder_done = 0;
static volatile int sig_holder_release = 0;
static volatile int sig_contender_done = 0;
static volatile int sig_contender_acquired = 0;
static struct task *sig_contender_task = NULL;

static void task_mutex_sig_holder(void)
{
    kmutex_lock(&test_contend_mutex);
    while (!sig_holder_release) {
        sched_sleep_ms(5);
    }
    sig_holder_done = 1;
    kmutex_unlock(&test_contend_mutex);
}

static void task_mutex_sig_contender(void)
{
    sig_contender_task = sched_current_task();
    kmutex_lock(&test_contend_mutex);
    sig_contender_acquired = 1;
    sig_contender_done = 1;
    kmutex_unlock(&test_contend_mutex);
}

#define SD_CONTEND_TASKS 4
#define SD_CONTEND_READS 20

static volatile int sd_contend_done[SD_CONTEND_TASKS];
static volatile int sd_contend_fail[SD_CONTEND_TASKS];
static uint8_t sd_contend_buf[SD_CONTEND_TASKS][512];
static volatile int sd_next_task_id = 0;

static void task_sd_contender(void)
{
    int id = __atomic_fetch_add(&sd_next_task_id, 1, __ATOMIC_SEQ_CST);
    if (id >= SD_CONTEND_TASKS) {
        return;
    }
    struct block_device *dev = block_device_lookup("sd0");
    if (!dev) {
        sd_contend_fail[id] = 1;
        sd_contend_done[id] = 1;
        return;
    }

    for (int i = 0; i < SD_CONTEND_READS; i++) {
        memset(sd_contend_buf[id], 0, 512);
        int res = sd_read_blocks(dev, sd_contend_buf[id], 0, 1);
        if (res != 0) {
            sd_contend_fail[id] = 1;
            break;
        }
        if (sd_contend_buf[id][510] != 0x55 || sd_contend_buf[id][511] != 0xAA) {
            sd_contend_fail[id] = 1;
            break;
        }
    }
    sd_contend_done[id] = 1;
}

static volatile int pipe_test_done = 0;
static volatile int pipe_test_bytes = 0;
static char pipe_test_buf[64];
static int pipe_test_fds[2] = {-1, -1};
static struct task *pipe_test_reader = NULL;

static void task_pipe_reader(void)
{
    pipe_test_reader = sched_current_task();
    memset(pipe_test_buf, 0, sizeof(pipe_test_buf));
    pipe_test_bytes = vfs_read(pipe_test_fds[0], pipe_test_buf, sizeof(pipe_test_buf));
    pipe_test_done = 1;
}

static volatile int tty_test_done = 0;
static volatile int tty_test_bytes = 0;
static char tty_test_buf[16];
static struct task *tty_test_reader_task = NULL;

static struct wait_queue timed_wq = WAIT_QUEUE_INIT;
static volatile int timed_cond = 0;
static volatile int timed_done = 0;
static volatile int timed_ret = 0;
static volatile int timed_left_armed = 0;
static struct task *timed_task = NULL;

static void task_timed_wait(void)
{
    timed_task = sched_current_task();
    timed_ret = wq_wait_event_timeout(&timed_wq, timed_cond != 0, 2000);
    timed_left_armed = sched_test_in_sleep_queue(sched_current_task());
    timed_done = 1;
}

static void task_tty_reader(void)
{
    tty_test_reader_task = sched_current_task();
    memset(tty_test_buf, 0, sizeof(tty_test_buf));
    tty_test_bytes = tty_read(&console_tty, NULL, tty_test_buf, 4);
    tty_test_done = 1;
}

void test_wait_scheduler(void)
{
    TEST_SUITE_BEGIN("WaitQueue-MultiTask");

    // 1. Waking a task that is really asleep
    {
        wq_init(&sched_test_wq);
        sched_test_cond = 0;
        sched_test_done = 0;
        sched_test_task_ptr = NULL;

        sched_create_task(task_sleep_wait);
        WAIT_UNTIL(task_blocked(sched_test_task_ptr));

        TEST_ASSERT("sleeper task is waiting", sched_test_done == 0);
        TEST_ASSERT("sleeper is in wq", sched_test_wq.head != NULL);

        sched_test_cond = 1;
        int woke = wq_wake_one(&sched_test_wq);
        TEST_ASSERT_EQ("wake_one returned 1", (long)woke, 1);

        WAIT_UNTIL(sched_test_done);
        TEST_ASSERT("sleeper woke and finished", sched_test_done == 1);
        TEST_ASSERT("wq is empty after sleep", sched_test_wq.head == NULL);
    }

    // 2. Spurious wakeup: condition still false, task goes back to sleep
    {
        TEST_ASSERT("queue empty before reuse", sched_test_wq.head == NULL);
        sched_test_cond = 0;
        sched_test_done = 0;
        sched_test_task_ptr = NULL;

        sched_create_task(task_sleep_wait);
        WAIT_UNTIL(task_blocked(sched_test_task_ptr));

        TEST_ASSERT("spurious sleeper is waiting", sched_test_done == 0);
        TEST_ASSERT("spurious sleeper in wq", sched_test_wq.head != NULL);

        // Wake without setting condition
        wq_wake_one(&sched_test_wq);
        WAIT_UNTIL(task_blocked(sched_test_task_ptr) && sched_test_wq.head != NULL);

        TEST_ASSERT("sleeper stayed asleep on spurious wake", sched_test_done == 0);
        TEST_ASSERT("sleeper still in wq after spurious wake", sched_test_wq.head != NULL);

        // Real wake with condition true
        sched_test_cond = 1;
        wq_wake_one(&sched_test_wq);
        WAIT_UNTIL(sched_test_done);

        TEST_ASSERT("sleeper finished after real wake", sched_test_done == 1);
        TEST_ASSERT("wq empty after spurious test", sched_test_wq.head == NULL);
    }

    // 3. Signal interrupt in wq_wait_event_interruptible
    {
        TEST_ASSERT("queue empty before reuse", sched_test_wq.head == NULL);
        sched_test_cond = 0;
        sched_test_done = 0;
        sched_test_ret = 0;
        sched_test_task_ptr = NULL;

        sched_create_task(task_sleep_interruptible);
        WAIT_UNTIL(task_blocked(sched_test_task_ptr));

        TEST_ASSERT("interruptible sleeper is waiting", sched_test_done == 0);
        TEST_ASSERT("sleeper in wq", sched_test_wq.head != NULL);

        // Mark signal pending on kernel process (pid 0) with atomic OR
        struct process *kproc = process_table[0];
        TEST_ASSERT("kproc exists", kproc != NULL);
        if (kproc) {
            __atomic_fetch_or(&kproc->pending_signals, 1u << (SIGUSR1 - 1), __ATOMIC_SEQ_CST);
        }

        // Real signal unblocks the task without popping from wq
        if (sched_test_task_ptr) {
            sched_unblock(sched_test_task_ptr);
        }
        WAIT_UNTIL(sched_test_done);

        TEST_ASSERT("interruptible sleeper finished", sched_test_done == 1);
        TEST_ASSERT_EQ("interruptible wait returned -ERESTARTSYS", (long)sched_test_ret,
                       (long)-ERESTARTSYS);
        TEST_ASSERT("wq empty after signal interrupt", sched_test_wq.head == NULL);

        if (kproc) {
            __atomic_fetch_and(&kproc->pending_signals, ~(1u << (SIGUSR1 - 1)), __ATOMIC_SEQ_CST);
        }
    }

    // 4. An uninterruptible wait that ignores a signal
    {
        TEST_ASSERT("queue empty before reuse", sched_test_wq.head == NULL);
        sched_test_cond = 0;
        sched_test_done = 0;
        sched_test_task_ptr = NULL;

        sched_create_task(task_sleep_wait);
        WAIT_UNTIL(task_blocked(sched_test_task_ptr));

        TEST_ASSERT("uninterruptible task is waiting", sched_test_done == 0);

        // Mark signal pending with atomic OR
        struct process *kproc = process_table[0];
        if (kproc) {
            __atomic_fetch_or(&kproc->pending_signals, 1u << (SIGUSR1 - 1), __ATOMIC_SEQ_CST);
        }

        // Unblock task while condition is still 0
        if (sched_test_task_ptr) {
            sched_unblock(sched_test_task_ptr);
        }
        WAIT_UNTIL(task_blocked(sched_test_task_ptr));

        TEST_ASSERT("uninterruptible task ignored signal", sched_test_done == 0);
        TEST_ASSERT("uninterruptible task still in wq", sched_test_wq.head != NULL);

        // Now satisfy condition and wake
        sched_test_cond = 1;
        wq_wake_one(&sched_test_wq);
        WAIT_UNTIL(sched_test_done);

        TEST_ASSERT("uninterruptible task completed on condition", sched_test_done == 1);
        TEST_ASSERT("wq is empty", sched_test_wq.head == NULL);

        if (kproc) {
            __atomic_fetch_and(&kproc->pending_signals, ~(1u << (SIGUSR1 - 1)), __ATOMIC_SEQ_CST);
        }
    }

    // 5. Regression test: task never overwrites its own READY state
    {
        regression_done = 0;
        sched_create_task(task_regression_ready_overwrite);
        WAIT_UNTIL(regression_done);
        TEST_ASSERT("regression test completed without loop", regression_done == 1);
    }

    // 6. Deterministic wake pass-on when woken task is signal-interrupted
    {
        wq_init(&passon_wq);
        passon_token = 0;
        passon_cond_armed = 0;
        passon_task_a_done = 0;
        passon_task_a_ret = 0;
        passon_task_b_done = 0;
        passon_task_a = NULL;
        passon_task_b = NULL;

        sched_create_task(task_passon_a);
        WAIT_UNTIL(task_blocked(passon_task_a));

        TEST_ASSERT("task A is waiting", passon_task_a_done == 0);
        TEST_ASSERT("task A is in wq", passon_wq.head != NULL);

        sched_create_task(task_passon_b);
        WAIT_UNTIL(task_blocked(passon_task_b));

        TEST_ASSERT("task B is waiting", passon_task_b_done == 0);
        TEST_ASSERT("both tasks in wq", passon_wq.head != passon_wq.tail);

        // Arm condition and unblock task A to evaluate condition
        passon_cond_armed = 1;
        if (passon_task_a) {
            sched_unblock(passon_task_a);
        }
        WAIT_UNTIL(passon_task_a_done && passon_task_b_done);

        TEST_ASSERT("task A finished", passon_task_a_done == 1);
        TEST_ASSERT_EQ("task A returned -ERESTARTSYS", (long)passon_task_a_ret, (long)-ERESTARTSYS);
        TEST_ASSERT("task B woke and finished", passon_task_b_done == 1);
        TEST_ASSERT("passon_wq is empty", passon_wq.head == NULL);

        struct process *kproc = process_table[0];
        if (kproc) {
            __atomic_fetch_and(&kproc->pending_signals, ~(1u << (SIGUSR1 - 1)), __ATOMIC_SEQ_CST);
        }
    }

    // 7. Contended kmutex with multiple tasks
    {
        kmutex_init(&test_contend_mutex);
        mutex_cs_count = 0;
        mutex_cs_violation = 0;
        holder_done = 0;
        holder_release = 0;
        contender1_done = 0;
        contender2_done = 0;
        contender_order_idx = 0;
        contender_order[0] = 0;
        contender_order[1] = 0;
        contender1_task = NULL;
        contender2_task = NULL;

        // i. Holder takes the mutex
        sched_create_task(task_contend_holder);
        WAIT_UNTIL(test_contend_mutex.depth == 1);

        TEST_ASSERT("holder has mutex", test_contend_mutex.depth == 1);
        TEST_ASSERT("holder in progress", holder_done == 0);

        // ii. Two contenders block on it
        sched_create_task(task_contender1);
        WAIT_UNTIL(task_blocked(contender1_task));
        TEST_ASSERT("contender 1 blocked", contender1_done == 0);
        TEST_ASSERT("contender 1 queued in wq", test_contend_mutex.wq.head != NULL);

        sched_create_task(task_contender2);
        WAIT_UNTIL(task_blocked(contender2_task));
        TEST_ASSERT("contender 2 blocked", contender2_done == 0);
        TEST_ASSERT("both contenders queued in wq",
                    test_contend_mutex.wq.head != test_contend_mutex.wq.tail);

        // Signal holder to release mutex
        holder_release = 1;

        WAIT_UNTIL(holder_done && contender1_done && contender2_done);

        // iii. After the unlock, each contender gets the mutex in turn
        TEST_ASSERT("holder finished", holder_done == 1);
        TEST_ASSERT("contender 1 finished", contender1_done == 1);
        TEST_ASSERT("contender 2 finished", contender2_done == 1);
        TEST_ASSERT("both contenders ran in turn", contender_order_idx == 2);
        TEST_ASSERT("the mutex was handed over in queue order",
                    contender_order[0] == 1 && contender_order[1] == 2);

        // iv. A counter protected by the mutex never sees two holders at once
        TEST_ASSERT("counter never saw two holders at once", mutex_cs_violation == 0);
        TEST_ASSERT("mutex fully unlocked",
                    test_contend_mutex.depth == 0 && test_contend_mutex.owner == NULL);
        TEST_ASSERT("mutex wq empty", test_contend_mutex.wq.head == NULL);
    }

    // 8. kmutex_lock blocks and succeeds even with a pending signal
    {
        TEST_ASSERT("mutex free before reuse",
                    test_contend_mutex.depth == 0 && test_contend_mutex.wq.head == NULL);
        sig_holder_done = 0;
        sig_holder_release = 0;
        sig_contender_done = 0;
        sig_contender_acquired = 0;
        sig_contender_task = NULL;

        sched_create_task(task_mutex_sig_holder);
        WAIT_UNTIL(test_contend_mutex.depth == 1);

        TEST_ASSERT("sig holder owns mutex", test_contend_mutex.depth == 1);

        sched_create_task(task_mutex_sig_contender);
        WAIT_UNTIL(task_blocked(sig_contender_task));

        TEST_ASSERT("contender blocked", sig_contender_done == 0);
        TEST_ASSERT("contender in mutex wq", test_contend_mutex.wq.head != NULL);
        TEST_ASSERT("contender task recorded", sig_contender_task != NULL);

        // Mark signal pending with atomic OR
        struct process *kproc = process_table[0];
        if (kproc) {
            __atomic_fetch_or(&kproc->pending_signals, 1u << (SIGUSR1 - 1), __ATOMIC_SEQ_CST);
        }

        // Unblock contender while holder still owns mutex
        if (sig_contender_task) {
            sched_unblock(sig_contender_task);
        }
        WAIT_UNTIL(task_blocked(sig_contender_task));

        TEST_ASSERT("contender ignored signal wake, still blocked", sig_contender_done == 0);
        TEST_ASSERT("contender still in mutex wq", test_contend_mutex.wq.head != NULL);
        TEST_ASSERT("contender has not acquired mutex", sig_contender_acquired == 0);
        TEST_ASSERT("holder still owns mutex", test_contend_mutex.depth == 1);

        // Signal holder to release mutex
        sig_holder_release = 1;

        WAIT_UNTIL(sig_holder_done && sig_contender_done);

        TEST_ASSERT("sig holder finished", sig_holder_done == 1);
        TEST_ASSERT("contender succeeded after blocking", sig_contender_done == 1);
        TEST_ASSERT("contender acquired mutex", sig_contender_acquired == 1);
        TEST_ASSERT("mutex unlocked", test_contend_mutex.depth == 0);
        TEST_ASSERT("mutex wq empty", test_contend_mutex.wq.head == NULL);

        if (kproc) {
            __atomic_fetch_and(&kproc->pending_signals, ~(1u << (SIGUSR1 - 1)), __ATOMIC_SEQ_CST);
        }
    }

    // 9. Contended SD driver I/O across 4 tasks
    {
        sd_next_task_id = 0;
        for (int i = 0; i < SD_CONTEND_TASKS; i++) {
            sd_contend_done[i] = 0;
            sd_contend_fail[i] = 0;
        }

        for (int i = 0; i < SD_CONTEND_TASKS; i++) {
            sched_create_task(task_sd_contender);
        }

        WAIT_UNTIL(sd_contend_done[0] && sd_contend_done[1] && sd_contend_done[2]
                   && sd_contend_done[3]);

        TEST_ASSERT("sd task 0 finished", sd_contend_done[0] == 1);
        TEST_ASSERT("sd task 1 finished", sd_contend_done[1] == 1);
        TEST_ASSERT("sd task 2 finished", sd_contend_done[2] == 1);
        TEST_ASSERT("sd task 3 finished", sd_contend_done[3] == 1);
        TEST_ASSERT("sd task 0 read succeeded", sd_contend_fail[0] == 0);
        TEST_ASSERT("sd task 1 read succeeded", sd_contend_fail[1] == 0);
        TEST_ASSERT("sd task 2 read succeeded", sd_contend_fail[2] == 0);
        TEST_ASSERT("sd task 3 read succeeded", sd_contend_fail[3] == 0);
    }

    // 10. Pipe read blocks on empty pipe and wakes on write
    {
        pipe_test_done = 0;
        pipe_test_bytes = -1;
        pipe_test_reader = NULL;
        TEST_ASSERT_EQ("create pipe for blocked reader", pipe_create(pipe_test_fds), 0);

        sched_create_task(task_pipe_reader);
        WAIT_UNTIL(task_blocked(pipe_test_reader));

        TEST_ASSERT("reader is blocked on empty pipe", task_blocked(pipe_test_reader));

        static const char msg[] = "pipe wakeup test";
        int len = (int)sizeof(msg);
        TEST_ASSERT_EQ("write payload to pipe", vfs_write(pipe_test_fds[1], msg, len), len);

        WAIT_UNTIL(pipe_test_done);

        TEST_ASSERT("reader finished", pipe_test_done == 1);
        TEST_ASSERT_EQ("reader read correct byte count", pipe_test_bytes, len);
        TEST_ASSERT("reader received correct bytes", memcmp(pipe_test_buf, msg, len) == 0);

        vfs_close(pipe_test_fds[0]);
        vfs_close(pipe_test_fds[1]);
    }

    // 11. Pipe read blocks on empty pipe and wakes with EOF on writer close
    {
        pipe_test_done = 0;
        pipe_test_bytes = -1;
        pipe_test_reader = NULL;
        TEST_ASSERT_EQ("create pipe for EOF test", pipe_create(pipe_test_fds), 0);

        sched_create_task(task_pipe_reader);
        WAIT_UNTIL(task_blocked(pipe_test_reader));

        TEST_ASSERT("reader is blocked before EOF", task_blocked(pipe_test_reader));

        TEST_ASSERT_EQ("close writer to trigger EOF", vfs_close(pipe_test_fds[1]), 0);

        WAIT_UNTIL(pipe_test_done);

        TEST_ASSERT("reader woke on writer close", pipe_test_done == 1);
        TEST_ASSERT_EQ("reader got 0 for EOF", pipe_test_bytes, 0);

        vfs_close(pipe_test_fds[0]);
    }

    // 12. TTY reader blocks on empty input and wakes on received byte
    {
        unsigned long flags = spin_lock_irqsave(&console_tty.lock);
        console_tty.rx_head = 0;
        console_tty.rx_tail = 0;
        spin_unlock_irqrestore(&console_tty.lock, flags);

        tty_test_done = 0;
        tty_test_bytes = 0;
        tty_test_reader_task = NULL;

        sched_create_task(task_tty_reader);
        WAIT_UNTIL(task_blocked(tty_test_reader_task) && console_tty.rx_wq.head != NULL);

        TEST_ASSERT("tty reader is blocked", tty_test_done == 0);
        TEST_ASSERT("tty reader is in rx_wq", console_tty.rx_wq.head != NULL);

        tty_handle_rx(&console_tty, 'a');

        WAIT_UNTIL(tty_test_done);

        TEST_ASSERT("tty reader woke and finished", tty_test_done == 1);
        TEST_ASSERT_EQ("tty reader returned 1 byte", tty_test_bytes, 1);
        TEST_ASSERT("tty reader received character 'a'", tty_test_buf[0] == 'a');
    }

    // 13. TTY reader blocks and returns -ERESTARTSYS on signal wake
    {
        tty_test_done = 0;
        tty_test_bytes = 0;
        tty_test_reader_task = NULL;

        struct process *kproc = process_table[0];

        sched_create_task(task_tty_reader);
        WAIT_UNTIL(task_blocked(tty_test_reader_task) && console_tty.rx_wq.head != NULL);

        TEST_ASSERT("tty reader is blocked before signal", tty_test_done == 0);
        TEST_ASSERT("tty reader is in rx_wq", console_tty.rx_wq.head != NULL);
        TEST_ASSERT("tty reader task ptr set", tty_test_reader_task != NULL);

        if (kproc) {
            __atomic_fetch_or(&kproc->pending_signals, 1u << (SIGUSR1 - 1), __ATOMIC_SEQ_CST);
        }
        if (tty_test_reader_task) {
            sched_unblock(tty_test_reader_task);
        }

        WAIT_UNTIL(tty_test_done);

        TEST_ASSERT("tty reader woke on signal", tty_test_done == 1);
        TEST_ASSERT_EQ("tty reader returned -ERESTARTSYS", tty_test_bytes, -ERESTARTSYS);

        if (kproc) {
            __atomic_fetch_and(&kproc->pending_signals, ~(1u << (SIGUSR1 - 1)), __ATOMIC_SEQ_CST);
        }

        unsigned long flags = spin_lock_irqsave(&console_tty.lock);
        console_tty.rx_head = 0;
        console_tty.rx_tail = 0;
        spin_unlock_irqrestore(&console_tty.lock, flags);
    }

    // 14. A timed wait on a condition that never holds gives up at its deadline
    {
        struct wait_queue wq = WAIT_QUEUE_INIT;
        unsigned long start = timer_get_system_time();
        int r = wq_wait_event_timeout(&wq, 0, 30);
        unsigned long took = timer_get_system_time() - start;

        TEST_ASSERT_EQ("timed wait returns -ETIMEDOUT", (long)r, (long)-ETIMEDOUT);
        TEST_ASSERT("timed wait lasted until its deadline", took >= 30);
        TEST_ASSERT("timed-out waiter left the queue", wq.head == NULL);
        TEST_ASSERT("timed-out waiter left no timer armed",
                    !sched_test_in_sleep_queue(sched_current_task()));
    }

    // 15. A wake before the deadline ends a timed wait with 0 and disarms the timer
    {
        timed_cond = 0;
        timed_done = 0;
        timed_ret = 1;
        timed_left_armed = 1;
        timed_task = NULL;

        sched_create_task(task_timed_wait);
        WAIT_UNTIL(task_blocked(timed_task));
        TEST_ASSERT("timed waiter is asleep", timed_done == 0);

        timed_cond = 1;
        wq_wake_one(&timed_wq);
        WAIT_UNTIL(timed_done);

        TEST_ASSERT("timed waiter finished", timed_done == 1);
        TEST_ASSERT_EQ("timed wait woken early returns 0", (long)timed_ret, 0);
        TEST_ASSERT("woken timed waiter left no timer armed", timed_left_armed == 0);
    }

    TEST_SUITE_END("WaitQueue-MultiTask");
}
