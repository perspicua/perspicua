/*
 * test_semaphore.c - Tests for counting semaphores (ksem).
 */

#include "core/semaphore.h"

#include <stddef.h>

#include "core/signals.h"
#include "core/timer.h"
#include "sched/process.h"
#include "sched/sched.h"
#include "sched/wait.h"
#include "test.h"
#include "uapi/errno.h"

static struct ksem sem_test = KSEM_INIT(0);

void test_semaphore(void)
{
    TEST_SUITE_BEGIN("Semaphore");

    // 1. Initialisation sets count and leaves wait queue empty
    {
        TEST_ASSERT("sem: queue empty before init", sem_test.wq.head == NULL);
        ksem_init(&sem_test, 3);
        TEST_ASSERT_EQ("sem: init count 3", sem_test.count, 3);
        TEST_ASSERT("sem: init wq head null", sem_test.wq.head == NULL);
        TEST_ASSERT("sem: init wq tail null", sem_test.wq.tail == NULL);
    }

    // 2. Static initialiser matches runtime initialisation
    {
        struct ksem statically = KSEM_INIT(2);
        TEST_ASSERT("sem: queue empty before re-init", sem_test.wq.head == NULL);
        ksem_init(&sem_test, 2);
        TEST_ASSERT_EQ("sem: static init count", statically.count, 2);
        TEST_ASSERT("sem: static init wq head null", statically.wq.head == NULL);
        TEST_ASSERT("sem: static init wq tail null", statically.wq.tail == NULL);
        TEST_ASSERT_EQ("sem: init matches static count", sem_test.count, statically.count);
        TEST_ASSERT("sem: init wq head matches static", sem_test.wq.head == statically.wq.head);
        TEST_ASSERT("sem: init wq tail matches static", sem_test.wq.tail == statically.wq.tail);
    }

    // 3. Draining permits via trydown stops at zero without underflow
    {
        TEST_ASSERT("sem: queue empty before re-init", sem_test.wq.head == NULL);
        ksem_init(&sem_test, 3);
        TEST_ASSERT_EQ("sem: trydown 1 returns 1", ksem_trydown(&sem_test), 1);
        TEST_ASSERT_EQ("sem: trydown 2 returns 1", ksem_trydown(&sem_test), 1);
        TEST_ASSERT_EQ("sem: trydown 3 returns 1", ksem_trydown(&sem_test), 1);
        TEST_ASSERT_EQ("sem: trydown 4 returns 0", ksem_trydown(&sem_test), 0);
        TEST_ASSERT_EQ("sem: count is 0 after drain", sem_test.count, 0);
    }

    // 4. Refilling via up increments count and leaves queue untouched
    {
        ksem_up(&sem_test);
        TEST_ASSERT_EQ("sem: up gives count 1", sem_test.count, 1);
        TEST_ASSERT("sem: up leaves wq head null", sem_test.wq.head == NULL);
        TEST_ASSERT("sem: up leaves wq tail null", sem_test.wq.tail == NULL);
        TEST_ASSERT_EQ("sem: trydown after refill returns 1", ksem_trydown(&sem_test), 1);
        TEST_ASSERT_EQ("sem: count is 0 after refill trydown", sem_test.count, 0);
    }

    // 5. Uncontended fast paths acquire immediately without blocking
    {
        TEST_ASSERT("sem: queue empty before re-init", sem_test.wq.head == NULL);
        ksem_init(&sem_test, 1);
        ksem_down(&sem_test);
        TEST_ASSERT_EQ("sem: fast down leaves count 0", sem_test.count, 0);

        TEST_ASSERT("sem: queue empty before re-init", sem_test.wq.head == NULL);
        ksem_init(&sem_test, 1);
        TEST_ASSERT_EQ("sem: fast down interruptible returns 0", ksem_down_interruptible(&sem_test),
                       0);
        TEST_ASSERT_EQ("sem: fast down interruptible leaves count 0", sem_test.count, 0);

        TEST_ASSERT("sem: queue empty before re-init", sem_test.wq.head == NULL);
        ksem_init(&sem_test, 1);
        TEST_ASSERT_EQ("sem: fast down timeout returns 0", ksem_down_timeout(&sem_test, 1000), 0);
        TEST_ASSERT_EQ("sem: fast down timeout leaves count 0", sem_test.count, 0);
    }

    // 6. Zero timeout at count 0 returns -ETIMEDOUT without joining queue
    {
        TEST_ASSERT("sem: queue empty before re-init", sem_test.wq.head == NULL);
        ksem_init(&sem_test, 0);
        TEST_ASSERT_EQ("sem: zero timeout returns -ETIMEDOUT", ksem_down_timeout(&sem_test, 0),
                       -ETIMEDOUT);
        TEST_ASSERT_EQ("sem: zero timeout leaves count 0", sem_test.count, 0);
        TEST_ASSERT("sem: zero timeout leaves queue empty", sem_test.wq.head == NULL);
    }

    TEST_SUITE_END("Semaphore");
}

/*
 * Multi-task scheduler tests.
 */

static struct task *one_waiter_task = NULL;
static volatile int one_waiter_done = 0;

static void task_one_waiter(void)
{
    one_waiter_task = sched_current_task();
    ksem_down(&sem_test);
    one_waiter_done = 1;
}

#define MAX_MULTI_WAITERS 4

static struct task *waiters_tasks[MAX_MULTI_WAITERS];
static volatile int waiters_done[MAX_MULTI_WAITERS];
static volatile int waiters_next_id = 0;

static void task_multi_waiter(void)
{
    int id = __atomic_fetch_add(&waiters_next_id, 1, __ATOMIC_SEQ_CST);
    if (id >= MAX_MULTI_WAITERS) {
        return;
    }
    waiters_tasks[id] = sched_current_task();
    ksem_down(&sem_test);
    waiters_done[id] = 1;
}

static int waiters_all_blocked(int n)
{
    for (int i = 0; i < n; i++) {
        if (!task_blocked(waiters_tasks[i])) {
            return 0;
        }
    }
    return 1;
}

static int waiters_all_done(int n)
{
    for (int i = 0; i < n; i++) {
        if (!waiters_done[i]) {
            return 0;
        }
    }
    return 1;
}

static int waiters_count_done(int n)
{
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (waiters_done[i]) {
            count++;
        }
    }
    return count;
}

static int waiters_count_blocked(int n)
{
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (!waiters_done[i] && task_blocked(waiters_tasks[i])) {
            count++;
        }
    }
    return count;
}

static void run_waiters(int n_waiters, int n_ups, int expect_count)
{
    TEST_ASSERT("sem: queue empty before re-init", sem_test.wq.head == NULL);
    ksem_init(&sem_test, 0);

    waiters_next_id = 0;
    for (int i = 0; i < n_waiters; i++) {
        waiters_tasks[i] = NULL;
        waiters_done[i] = 0;
    }

    for (int i = 0; i < n_waiters; i++) {
        sched_create_task(task_multi_waiter);
    }

    WAIT_UNTIL(waiters_all_blocked(n_waiters));
    TEST_ASSERT("sem: all waiters blocked", waiters_all_blocked(n_waiters));

    for (int i = 0; i < n_ups; i++) {
        ksem_up(&sem_test);
    }

    if (n_ups >= n_waiters) {
        WAIT_UNTIL(waiters_all_done(n_waiters));
        TEST_ASSERT("sem: all waiters finished", waiters_all_done(n_waiters));
        TEST_ASSERT_EQ("sem: expected count matches", sem_test.count, expect_count);
        TEST_ASSERT("sem: queue is empty", sem_test.wq.head == NULL);
    } else {
        WAIT_UNTIL(waiters_count_done(n_waiters) == n_ups);
        TEST_ASSERT_EQ("sem: expected waiters finished", waiters_count_done(n_waiters), n_ups);

        sched_sleep_ms(50);
        TEST_ASSERT_EQ("sem: expected waiters still finished after sleep",
                       waiters_count_done(n_waiters), n_ups);
        TEST_ASSERT_EQ("sem: other waiters stay blocked", waiters_count_blocked(n_waiters),
                       n_waiters - n_ups);
        TEST_ASSERT_EQ("sem: count is 0 with blocked waiters", sem_test.count, 0);

        for (int i = 0; i < n_waiters - n_ups; i++) {
            ksem_up(&sem_test);
        }

        WAIT_UNTIL(waiters_all_done(n_waiters));
        TEST_ASSERT("sem: all waiters finished after drain", waiters_all_done(n_waiters));
        TEST_ASSERT_EQ("sem: expected count after drain", sem_test.count, expect_count);
        TEST_ASSERT("sem: queue is empty after drain", sem_test.wq.head == NULL);
    }
}

static struct task *timed_task = NULL;
static volatile int timed_done = 0;
static volatile int timed_ret = 0;
static volatile int timed_left_armed = 0;

static void task_timed_waiter(void)
{
    timed_task = sched_current_task();
    timed_ret = ksem_down_timeout(&sem_test, 2000);
    timed_left_armed = sched_test_in_sleep_queue(timed_task);
    timed_done = 1;
}

static struct task *intr_task = NULL;
static volatile int intr_done = 0;
static volatile int intr_ret = 0;

static void task_interruptible_waiter(void)
{
    intr_task = sched_current_task();
    intr_ret = ksem_down_interruptible(&sem_test);
    intr_done = 1;
}

#define HOLDER_LIMIT 2
#define HOLDER_TASKS 4
#define HOLDER_ITERS 20

static volatile int holders_in_cs = 0;
static volatile int holders_violation = 0;
static volatile int holders_max_seen = 0;
static volatile int holders_done[HOLDER_TASKS];
static volatile int holders_next_id = 0;

static void task_holder_contender(void)
{
    int id = __atomic_fetch_add(&holders_next_id, 1, __ATOMIC_SEQ_CST);
    if (id >= HOLDER_TASKS) {
        return;
    }

    for (int i = 0; i < HOLDER_ITERS; i++) {
        ksem_down(&sem_test);

        int n = __atomic_add_fetch(&holders_in_cs, 1, __ATOMIC_SEQ_CST);
        if (n > HOLDER_LIMIT) {
            __atomic_store_n(&holders_violation, 1, __ATOMIC_SEQ_CST);
        }
        int cur = __atomic_load_n(&holders_max_seen, __ATOMIC_RELAXED);
        while (n > cur) {
            if (__atomic_compare_exchange_n(&holders_max_seen, &cur, n, 0, __ATOMIC_SEQ_CST,
                                            __ATOMIC_RELAXED)) {
                break;
            }
        }

        sched_sleep_ms(1);

        __atomic_sub_fetch(&holders_in_cs, 1, __ATOMIC_SEQ_CST);

        ksem_up(&sem_test);
    }

    holders_done[id] = 1;
}

static int holders_all_done(void)
{
    for (int i = 0; i < HOLDER_TASKS; i++) {
        if (!holders_done[i]) {
            return 0;
        }
    }
    return 1;
}

void test_semaphore_scheduler(void)
{
    TEST_SUITE_BEGIN("Semaphore-MultiTask");

    // 1. Single waiter blocks and wakes when permit arrives
    {
        TEST_ASSERT("sem: queue empty before re-init", sem_test.wq.head == NULL);
        ksem_init(&sem_test, 0);
        one_waiter_task = NULL;
        one_waiter_done = 0;

        sched_create_task(task_one_waiter);
        WAIT_UNTIL(task_blocked(one_waiter_task));

        TEST_ASSERT("sem: waiter is not done while blocked", one_waiter_done == 0);
        TEST_ASSERT("sem: waiter is in wait queue", sem_test.wq.head != NULL);

        ksem_up(&sem_test);
        WAIT_UNTIL(one_waiter_done);

        TEST_ASSERT("sem: waiter finished", one_waiter_done == 1);
        TEST_ASSERT_EQ("sem: count is 0 after waiter wakes", sem_test.count, 0);
        TEST_ASSERT("sem: wait queue empty after waiter wakes", sem_test.wq.head == NULL);
    }

    // 2. Multiple waiters and ups with concurrent completions
    run_waiters(3, 3, 0);
    run_waiters(2, 3, 1);
    run_waiters(3, 1, 0);

    // 3. Timeout expires when no permit arrives
    {
        TEST_ASSERT("sem: queue empty before re-init", sem_test.wq.head == NULL);
        ksem_init(&sem_test, 0);

        unsigned long t0 = timer_get_system_time();
        int ret = ksem_down_timeout(&sem_test, 50);
        unsigned long dt = timer_get_system_time() - t0;

        TEST_ASSERT_EQ("sem: timeout returns -ETIMEDOUT", ret, -ETIMEDOUT);
        TEST_ASSERT("sem: timeout took at least 50 ms", dt >= 50);
        TEST_ASSERT("sem: timeout took less than 1000 ms", dt < 1000);
        TEST_ASSERT_EQ("sem: timeout leaves count 0", sem_test.count, 0);
        TEST_ASSERT("sem: timeout queue empty", sem_test.wq.head == NULL);
        TEST_ASSERT("sem: timeout cancelled sleep queue entry",
                    !sched_test_in_sleep_queue(sched_current_task()));
    }

    // 4. Permit arrives before timeout expires
    {
        TEST_ASSERT("sem: queue empty before re-init", sem_test.wq.head == NULL);
        ksem_init(&sem_test, 0);
        timed_task = NULL;
        timed_done = 0;
        timed_ret = -1;
        timed_left_armed = -1;

        sched_create_task(task_timed_waiter);
        WAIT_UNTIL(task_blocked(timed_task));

        TEST_ASSERT("sem: timed task blocked", task_blocked(timed_task));
        TEST_ASSERT("sem: timed task in queue", sem_test.wq.head != NULL);

        ksem_up(&sem_test);
        WAIT_UNTIL(timed_done);

        TEST_ASSERT("sem: timed task finished", timed_done == 1);
        TEST_ASSERT_EQ("sem: timed wait returns 0", timed_ret, 0);
        TEST_ASSERT_EQ("sem: timed wait count 0", sem_test.count, 0);
        TEST_ASSERT_EQ("sem: timed wait disarmed timer", timed_left_armed, 0);
        TEST_ASSERT("sem: timed wait queue empty", sem_test.wq.head == NULL);
    }

    // 5. Signal interrupts a blocked interruptible wait
    {
        TEST_ASSERT("sem: queue empty before re-init", sem_test.wq.head == NULL);
        ksem_init(&sem_test, 0);
        intr_task = NULL;
        intr_done = 0;
        intr_ret = 0;

        sched_create_task(task_interruptible_waiter);
        WAIT_UNTIL(task_blocked(intr_task));

        TEST_ASSERT("sem: signal waiter blocked", task_blocked(intr_task));
        TEST_ASSERT("sem: signal waiter in queue", sem_test.wq.head != NULL);

        struct process *kproc = process_table[0];
        if (kproc) {
            __atomic_fetch_or(&kproc->pending_signals, 1u << (SIGUSR1 - 1), __ATOMIC_SEQ_CST);
        }
        if (intr_task) {
            sched_unblock(intr_task);
        }

        WAIT_UNTIL(intr_done);

        TEST_ASSERT("sem: signal waiter finished", intr_done == 1);
        TEST_ASSERT_EQ("sem: signal interrupted wait returns -ERESTARTSYS", intr_ret, -ERESTARTSYS);
        TEST_ASSERT_EQ("sem: interrupted wait count untouched", sem_test.count, 0);
        TEST_ASSERT("sem: interrupted wait queue empty", sem_test.wq.head == NULL);

        if (kproc) {
            __atomic_fetch_and(&kproc->pending_signals, ~(1u << (SIGUSR1 - 1)), __ATOMIC_SEQ_CST);
        }
    }

    // 6. Permit and signal arrive simultaneously (condition checked before signal)
    {
        TEST_ASSERT("sem: queue empty before re-init", sem_test.wq.head == NULL);
        ksem_init(&sem_test, 0);
        intr_task = NULL;
        intr_done = 0;
        intr_ret = -1;

        sched_create_task(task_interruptible_waiter);
        WAIT_UNTIL(task_blocked(intr_task));

        TEST_ASSERT("sem: waiter blocked before wake", task_blocked(intr_task));
        TEST_ASSERT("sem: waiter in queue before wake", sem_test.wq.head != NULL);

        struct process *kproc = process_table[0];
        if (kproc) {
            __atomic_fetch_or(&kproc->pending_signals, 1u << (SIGUSR1 - 1), __ATOMIC_SEQ_CST);
        }
        ksem_up(&sem_test);

        WAIT_UNTIL(intr_done);

        TEST_ASSERT("sem: waiter finished after simultaneous wake", intr_done == 1);
        TEST_ASSERT_EQ("sem: unit taken despite signal returns 0", intr_ret, 0);
        TEST_ASSERT_EQ("sem: unit taken leaves count 0", sem_test.count, 0);
        TEST_ASSERT("sem: queue empty after simultaneous wake", sem_test.wq.head == NULL);

        if (kproc) {
            __atomic_fetch_and(&kproc->pending_signals, ~(1u << (SIGUSR1 - 1)), __ATOMIC_SEQ_CST);
        }
    }

    // 7. Counting semaphore limits concurrency to K simultaneous holders
    {
        TEST_ASSERT("sem: queue empty before re-init", sem_test.wq.head == NULL);
        ksem_init(&sem_test, HOLDER_LIMIT);

        holders_in_cs = 0;
        holders_violation = 0;
        holders_max_seen = 0;
        holders_next_id = 0;
        for (int i = 0; i < HOLDER_TASKS; i++) {
            holders_done[i] = 0;
        }

        for (int i = 0; i < HOLDER_TASKS; i++) {
            sched_create_task(task_holder_contender);
        }

        WAIT_UNTIL(holders_all_done());

        TEST_ASSERT("sem: all four tasks finished", holders_all_done());
        TEST_ASSERT_EQ("sem: violation is 0", holders_violation, 0);
        TEST_ASSERT_EQ("sem: max holders seen matches limit", holders_max_seen, HOLDER_LIMIT);
        TEST_ASSERT_EQ("sem: count is back to max holders", sem_test.count, HOLDER_LIMIT);
        TEST_ASSERT("sem: queue empty after holders test", sem_test.wq.head == NULL);
    }

    TEST_SUITE_END("Semaphore-MultiTask");
}
