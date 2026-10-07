/*
 * test_completion.c - Tests for completions (struct completion).
 */

#include "core/completion.h"

#include <stddef.h>

#include "core/signals.h"
#include "core/timer.h"
#include "sched/process.h"
#include "sched/sched.h"
#include "sched/wait.h"
#include "test.h"
#include "uapi/errno.h"

static struct completion compl_test = COMPLETION_INIT;

void test_completion(void)
{
    TEST_SUITE_BEGIN("Completion");

    // 1. Initialisation sets done to 0 and matches COMPLETION_INIT
    {
        struct completion statically = COMPLETION_INIT;
        TEST_ASSERT("completion: static init done is 0", statically.done == 0);
        TEST_ASSERT("completion: static init queue empty",
                    statically.wq.head == NULL && statically.wq.tail == NULL);
        TEST_ASSERT("completion: static init completion_done returns 0",
                    completion_done(&statically) == 0);

        TEST_ASSERT("completion: queue empty before init", compl_test.wq.head == NULL);
        completion_init(&compl_test);
        TEST_ASSERT_EQ("completion: init done is 0", (long)compl_test.done, 0);
        TEST_ASSERT("completion: init queue empty",
                    compl_test.wq.head == NULL && compl_test.wq.tail == NULL);
        TEST_ASSERT_EQ("completion: init completion_done returns 0", completion_done(&compl_test),
                       0);
        TEST_ASSERT_EQ("completion: init matches static done", compl_test.done, statically.done);
    }

    // 2. Fresh completion try_wait returns 0 without modifying done
    {
        TEST_ASSERT_EQ("completion: fresh try_wait returns 0", completion_try_wait(&compl_test), 0);
        TEST_ASSERT_EQ("completion: fresh done stays 0", (long)compl_test.done, 0);
    }

    // 3. Single complete allows exactly one wait
    {
        complete(&compl_test);
        TEST_ASSERT_EQ("completion: done is 1 after complete", (long)compl_test.done, 1);
        TEST_ASSERT_EQ("completion: completion_done is 1", completion_done(&compl_test), 1);
        TEST_ASSERT_EQ("completion: first try_wait returns 1", completion_try_wait(&compl_test), 1);
        TEST_ASSERT_EQ("completion: second try_wait returns 0", completion_try_wait(&compl_test),
                       0);
        TEST_ASSERT_EQ("completion: done back to 0", (long)compl_test.done, 0);
    }

    // 4. Completion counts permits rather than acting as a flag
    {
        complete(&compl_test);
        complete(&compl_test);
        complete(&compl_test);
        TEST_ASSERT_EQ("completion: done is 3 after 3 completes", (long)compl_test.done, 3);
        TEST_ASSERT_EQ("completion: try_wait 1 returns 1", completion_try_wait(&compl_test), 1);
        TEST_ASSERT_EQ("completion: try_wait 2 returns 1", completion_try_wait(&compl_test), 1);
        TEST_ASSERT_EQ("completion: try_wait 3 returns 1", completion_try_wait(&compl_test), 1);
        TEST_ASSERT_EQ("completion: try_wait 4 returns 0", completion_try_wait(&compl_test), 0);
        TEST_ASSERT_EQ("completion: done back to 0 after drain", (long)compl_test.done, 0);
    }

    // 5. complete_all acts as an open latch that subsequent complete does not close
    {
        complete_all(&compl_test);
        for (int i = 0; i < 5; i++) {
            TEST_ASSERT_EQ("completion: complete_all try_wait succeeds",
                           completion_try_wait(&compl_test), 1);
        }
        TEST_ASSERT_EQ("completion: done stays COMPLETION_ALL", compl_test.done, COMPLETION_ALL);

        complete(&compl_test);
        TEST_ASSERT_EQ("completion: done remains COMPLETION_ALL after complete", compl_test.done,
                       COMPLETION_ALL);
    }

    // 6. reinit closes the latch after complete_all
    {
        TEST_ASSERT("completion: queue empty before reinit", compl_test.wq.head == NULL);
        completion_reinit(&compl_test);
        TEST_ASSERT_EQ("completion: done is 0 after reinit", (long)compl_test.done, 0);
        TEST_ASSERT_EQ("completion: try_wait returns 0 after reinit",
                       completion_try_wait(&compl_test), 0);
    }

    // 7. Uncontended fast paths after one complete each
    {
        complete(&compl_test);
        completion_wait(&compl_test);
        TEST_ASSERT_EQ("completion: wait consumed permit leaving done 0", (long)compl_test.done, 0);

        complete(&compl_test);
        TEST_ASSERT_EQ("completion: wait_interruptible returns 0",
                       completion_wait_interruptible(&compl_test), 0);
        TEST_ASSERT_EQ("completion: wait_interruptible consumed permit", (long)compl_test.done, 0);

        complete(&compl_test);
        TEST_ASSERT_EQ("completion: wait_timeout returns 0",
                       completion_wait_timeout(&compl_test, 1000), 0);
        TEST_ASSERT_EQ("completion: wait_timeout consumed permit", (long)compl_test.done, 0);
    }

    // 8. Zero timeout on pending completion returns -ETIMEDOUT without joining queue
    {
        TEST_ASSERT_EQ("completion: zero timeout returns -ETIMEDOUT",
                       completion_wait_timeout(&compl_test, 0), -ETIMEDOUT);
        TEST_ASSERT("completion: queue remains empty after zero timeout",
                    compl_test.wq.head == NULL);
        TEST_ASSERT_EQ("completion: done remains 0", (long)compl_test.done, 0);
    }

    TEST_SUITE_END("Completion");
}

/*
 * Multi-task scheduler tests.
 */

static struct task *one_waiter_task = NULL;
static volatile int one_waiter_done = 0;

static void task_one_waiter(void)
{
    one_waiter_task = sched_current_task();
    completion_wait(&compl_test);
    one_waiter_done = 1;
}

#define MAX_WAITERS 3

static struct task *waiters_tasks[MAX_WAITERS];
static volatile int waiters_done[MAX_WAITERS];
static volatile int waiters_next_id = 0;

static void task_multi_waiter(void)
{
    int id = __atomic_fetch_add(&waiters_next_id, 1, __ATOMIC_SEQ_CST);
    if (id >= MAX_WAITERS) {
        return;
    }
    waiters_tasks[id] = sched_current_task();
    completion_wait(&compl_test);
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

static struct task *intr_task = NULL;
static volatile int intr_done = 0;
static volatile int intr_ret = 0;

static void task_interruptible_waiter(void)
{
    intr_task = sched_current_task();
    intr_ret = completion_wait_interruptible(&compl_test);
    intr_done = 1;
}

void test_completion_scheduler(void)
{
    TEST_SUITE_BEGIN("Completion-MultiTask");

    // 1. Single blocked waiter woken by complete
    {
        TEST_ASSERT("completion: queue empty before reinit", compl_test.wq.head == NULL);
        completion_reinit(&compl_test);
        one_waiter_task = NULL;
        one_waiter_done = 0;

        sched_create_task(task_one_waiter);
        WAIT_UNTIL(task_blocked(one_waiter_task));

        TEST_ASSERT("completion: waiter is blocked", task_blocked(one_waiter_task));
        TEST_ASSERT("completion: waiter is in queue", compl_test.wq.head != NULL);

        complete(&compl_test);
        WAIT_UNTIL(one_waiter_done);

        TEST_ASSERT("completion: waiter finished", one_waiter_done == 1);
        TEST_ASSERT_EQ("completion: done is 0 after waiter wakes", (long)compl_test.done, 0);
        TEST_ASSERT("completion: queue empty after wake", compl_test.wq.head == NULL);
    }

    // 2. Wake-one wakes exactly one waiter, leaving the others blocked
    {
        TEST_ASSERT("completion: queue empty before reinit", compl_test.wq.head == NULL);
        completion_reinit(&compl_test);
        waiters_next_id = 0;
        for (int i = 0; i < MAX_WAITERS; i++) {
            waiters_tasks[i] = NULL;
            waiters_done[i] = 0;
        }

        for (int i = 0; i < MAX_WAITERS; i++) {
            sched_create_task(task_multi_waiter);
        }

        WAIT_UNTIL(waiters_all_blocked(MAX_WAITERS));
        TEST_ASSERT("completion: all 3 waiters blocked", waiters_all_blocked(MAX_WAITERS));

        complete(&compl_test);

        WAIT_UNTIL(waiters_count_done(MAX_WAITERS) == 1);
        TEST_ASSERT_EQ("completion: exactly one waiter finished", waiters_count_done(MAX_WAITERS),
                       1);

        sched_sleep_ms(50);
        TEST_ASSERT_EQ("completion: still exactly one waiter finished after sleep",
                       waiters_count_done(MAX_WAITERS), 1);
        TEST_ASSERT_EQ("completion: remaining 2 waiters stay blocked",
                       waiters_count_blocked(MAX_WAITERS), MAX_WAITERS - 1);
        TEST_ASSERT_EQ("completion: done is 0 with blocked waiters", (long)compl_test.done, 0);

        for (int i = 0; i < MAX_WAITERS - 1; i++) {
            complete(&compl_test);
        }

        WAIT_UNTIL(waiters_all_done(MAX_WAITERS));
        TEST_ASSERT("completion: all waiters finished after drain", waiters_all_done(MAX_WAITERS));
        TEST_ASSERT_EQ("completion: done is 0 after drain", (long)compl_test.done, 0);
        TEST_ASSERT("completion: queue empty after drain", compl_test.wq.head == NULL);
    }

    // 3. complete_all wakes all blocked waiters and keeps latch open
    {
        TEST_ASSERT("completion: queue empty before reinit", compl_test.wq.head == NULL);
        completion_reinit(&compl_test);
        waiters_next_id = 0;
        for (int i = 0; i < MAX_WAITERS; i++) {
            waiters_tasks[i] = NULL;
            waiters_done[i] = 0;
        }

        for (int i = 0; i < MAX_WAITERS; i++) {
            sched_create_task(task_multi_waiter);
        }

        WAIT_UNTIL(waiters_all_blocked(MAX_WAITERS));
        TEST_ASSERT("completion: all 3 waiters blocked before complete_all",
                    waiters_all_blocked(MAX_WAITERS));

        complete_all(&compl_test);

        WAIT_UNTIL(waiters_all_done(MAX_WAITERS));
        TEST_ASSERT("completion: all 3 waiters finished on complete_all",
                    waiters_all_done(MAX_WAITERS));
        TEST_ASSERT_EQ("completion: done stays COMPLETION_ALL", compl_test.done, COMPLETION_ALL);
        TEST_ASSERT("completion: queue empty after complete_all", compl_test.wq.head == NULL);

        // Later waiter returns immediately through open latch
        int later_ret = completion_wait_timeout(&compl_test, 1000);
        TEST_ASSERT_EQ("completion: later waiter returns 0 through open latch", later_ret, 0);
        TEST_ASSERT_EQ("completion: done still COMPLETION_ALL after later wait", compl_test.done,
                       COMPLETION_ALL);
    }

    // 4. Timeout expires and leaves timer unarmed
    {
        TEST_ASSERT("completion: queue empty before reinit", compl_test.wq.head == NULL);
        completion_reinit(&compl_test);

        unsigned long t0 = timer_get_system_time();
        int ret = completion_wait_timeout(&compl_test, 50);
        unsigned long dt = timer_get_system_time() - t0;

        TEST_ASSERT_EQ("completion: timeout returns -ETIMEDOUT", ret, -ETIMEDOUT);
        TEST_ASSERT("completion: timeout took at least 50 ms", dt >= 50);
        TEST_ASSERT("completion: timeout took less than 1000 ms", dt < 1000);
        TEST_ASSERT_EQ("completion: done is 0 after timeout", (long)compl_test.done, 0);
        TEST_ASSERT("completion: queue empty after timeout", compl_test.wq.head == NULL);
        TEST_ASSERT("completion: timer unarmed after timeout",
                    !sched_test_in_sleep_queue(sched_current_task()));
    }

    // 5. Signal interrupts a blocked wait
    {
        TEST_ASSERT("completion: queue empty before reinit", compl_test.wq.head == NULL);
        completion_reinit(&compl_test);
        intr_task = NULL;
        intr_done = 0;
        intr_ret = 0;

        sched_create_task(task_interruptible_waiter);
        WAIT_UNTIL(task_blocked(intr_task));

        TEST_ASSERT("completion: signal waiter blocked", task_blocked(intr_task));
        TEST_ASSERT("completion: signal waiter in queue", compl_test.wq.head != NULL);

        struct process *kproc = process_table[0];
        if (kproc) {
            __atomic_fetch_or(&kproc->pending_signals, 1u << (SIGUSR1 - 1), __ATOMIC_SEQ_CST);
        }
        if (intr_task) {
            sched_unblock(intr_task);
        }

        WAIT_UNTIL(intr_done);

        TEST_ASSERT("completion: signal waiter finished", intr_done == 1);
        TEST_ASSERT_EQ("completion: interrupted wait returns -ERESTARTSYS", intr_ret, -ERESTARTSYS);
        TEST_ASSERT_EQ("completion: done stays 0 after signal", (long)compl_test.done, 0);
        TEST_ASSERT("completion: queue empty after signal", compl_test.wq.head == NULL);

        if (kproc) {
            __atomic_fetch_and(&kproc->pending_signals, ~(1u << (SIGUSR1 - 1)), __ATOMIC_SEQ_CST);
        }
    }

    TEST_SUITE_END("Completion-MultiTask");
}
