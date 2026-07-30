#include "test.h"
#include "sched/sched.h"
#include "mm/heap.h"
#include "core/lock.h"
#include "core/timer.h"
#include "string.h"

// test constraints:
//   - tests run as the "main task" (task id 0) on core 0
//   - created tasks run asynchronously; use sched_sleep_ms to wait
//   - shared state needs spinlock protection
//   - test_assert uses return, so it can only be called from test_scheduler()

// shared test state

static volatile int counter_a = 0;
static volatile int counter_b = 0;
static volatile int counter_c = 0;
static spinlock_t test_lock = SPINLOCK_INIT;

// task functions

// basic: increment a counter
static void task_inc_a(void)
{
    unsigned long flags = spin_lock_irqsave(&test_lock);
    counter_a++;
    spin_unlock_irqrestore(&test_lock, flags);
}

static void task_inc_b(void)
{
    unsigned long flags = spin_lock_irqsave(&test_lock);
    counter_b++;
    spin_unlock_irqrestore(&test_lock, flags);
}

static void task_inc_c(void)
{
    unsigned long flags = spin_lock_irqsave(&test_lock);
    counter_c++;
    spin_unlock_irqrestore(&test_lock, flags);
}

// increment counter_a n times
static void task_inc_a_10x(void)
{
    for (int i = 0; i < 10; i++) {
        unsigned long flags = spin_lock_irqsave(&test_lock);
        counter_a++;
        spin_unlock_irqrestore(&test_lock, flags);
    }
}

// increment counter_a with small delay between increments
static void task_inc_a_with_delay(void)
{
    for (int i = 0; i < 5; i++) {
        unsigned long flags = spin_lock_irqsave(&test_lock);
        counter_a++;
        spin_unlock_irqrestore(&test_lock, flags);
        sched_sleep_ms(10);
    }
}

// task that sleeps then increments
static void task_sleep_then_inc(void)
{
    sched_sleep_ms(20);
    unsigned long flags = spin_lock_irqsave(&test_lock);
    counter_a++;
    spin_unlock_irqrestore(&test_lock, flags);
}

// task that does a longer sleep
static void task_long_sleep_then_inc(void)
{
    sched_sleep_ms(40);
    unsigned long flags = spin_lock_irqsave(&test_lock);
    counter_b++;
    spin_unlock_irqrestore(&test_lock, flags);
}

// publishes itself, then sleeps far longer than the suite runs so the test
// controls exactly when it wakes
static volatile int early_wake_ran = 0;
static struct task *early_wake_task = NULL;

static void task_publish_then_long_sleep(void)
{
    unsigned long flags = spin_lock_irqsave(&test_lock);
    early_wake_task = sched_get_current();
    spin_unlock_irqrestore(&test_lock, flags);

    sched_sleep_ms(10000);

    flags = spin_lock_irqsave(&test_lock);
    early_wake_ran = 1;
    spin_unlock_irqrestore(&test_lock, flags);
}

// task that writes a sequence to record execution order
static volatile int order_log[16];
static volatile int order_idx = 0;

static void task_order_1(void)
{
    unsigned long flags = spin_lock_irqsave(&test_lock);
    if (order_idx < 16)
        order_log[order_idx++] = 1;
    spin_unlock_irqrestore(&test_lock, flags);
}

static void task_order_2(void)
{
    unsigned long flags = spin_lock_irqsave(&test_lock);
    if (order_idx < 16)
        order_log[order_idx++] = 2;
    spin_unlock_irqrestore(&test_lock, flags);
}

static void task_order_3(void)
{
    unsigned long flags = spin_lock_irqsave(&test_lock);
    if (order_idx < 16)
        order_log[order_idx++] = 3;
    spin_unlock_irqrestore(&test_lock, flags);
}

// task that spawns another task
static void task_spawner(void)
{
    sched_create_task(task_inc_a);
    sched_create_task(task_inc_a);
}

// compute-bound task (no sleep/yield)
static volatile unsigned long compute_result = 0;

static void task_compute(void)
{
    unsigned long sum = 0;
    for (int i = 0; i < 10000; i++)
        sum += (unsigned long)i;
    compute_result = sum;
}

// stack canary task: uses some stack depth
static volatile int stack_test_ok = 0;

static void task_stack_depth(void)
{
    // Use some stack with local arrays
    volatile unsigned char buf[512];
    for (int i = 0; i < 512; i++)
        buf[i] = (unsigned char)(i & 0xFF);
    int ok = 1;
    for (int i = 0; i < 512; i++)
        if (buf[i] != (unsigned char)(i & 0xFF))
            ok = 0;
    stack_test_ok = ok;
}

// task that sleeps multiple times
static void task_multi_sleep(void)
{
    sched_sleep_ms(10);
    unsigned long flags = spin_lock_irqsave(&test_lock);
    counter_a++;
    spin_unlock_irqrestore(&test_lock, flags);

    sched_sleep_ms(10);
    flags = spin_lock_irqsave(&test_lock);
    counter_a++;
    spin_unlock_irqrestore(&test_lock, flags);

    sched_sleep_ms(10);
    flags = spin_lock_irqsave(&test_lock);
    counter_a++;
    spin_unlock_irqrestore(&test_lock, flags);
}

// timestamp recording task
static volatile unsigned long ts_short_done = 0;
static volatile unsigned long ts_long_done = 0;

static void task_short_sleep_ts(void)
{
    sched_sleep_ms(20);
    ts_short_done = get_system_time();
}

static void task_long_sleep_ts(void)
{
    sched_sleep_ms(40);
    ts_long_done = get_system_time();
}

// task that yields repeatedly
static volatile int yield_count = 0;
static void task_yield_loop(void)
{
    for (int i = 0; i < 50; i++) {
        unsigned long flags = spin_lock_irqsave(&test_lock);
        yield_count++;
        spin_unlock_irqrestore(&test_lock, flags);
        schedule(); // yield to other tasks
    }
}

// task that simulates the pipe_wait race: set state to BLOCKED, then yield
static volatile int race_task_ran = 0;
static struct task *race_wait_queue = NULL;

static void task_race_waiter(void)
{
    struct task *self = sched_get_current();
    unsigned long flags = irq_save();

    /* 1. Pre-mark as BLOCKED (simulating pipe_wait) */
    self->state = SCHED_TASK_BLOCKED;

    /* 2. Add to a "queue" so the unblocker can find us */
    unsigned long lock_flags = spin_lock_irqsave(&test_lock);
    race_wait_queue = self;
    spin_unlock_irqrestore(&test_lock, lock_flags);

    /* 3. Call schedule().  In the bug, if an interrupt or another core
     * unblocks us BEFORE we reach here, we enter schedule() with state=READY. */
    schedule();

    /* 4. If we survived, mark success */
    race_task_ran = 1;
    irq_restore(flags);
}

static void task_race_unblocker(void)
{
    // Wait for the waiter to put itself in the queue
    while (1) {
        unsigned long flags = spin_lock_irqsave(&test_lock);
        struct task *t = race_wait_queue;
        spin_unlock_irqrestore(&test_lock, flags);
        if (t)
            break;
        sched_sleep_ms(1);
    }

    // Unblock it
    unsigned long flags = spin_lock_irqsave(&test_lock);
    struct task *t = race_wait_queue;
    race_wait_queue = NULL;
    spin_unlock_irqrestore(&test_lock, flags);

    sched_unblock(t);
}

// test suite

void test_scheduler(void)
{
    TEST_SUITE_BEGIN("Scheduler");

    // unblock-before-schedule race test
    {
        race_task_ran = 0;
        race_wait_queue = NULL;
        sched_create_task(task_race_waiter);
        sched_create_task(task_race_unblocker);
        sched_sleep_ms(100);
        TEST_ASSERT("race task resumed correctly", race_task_ran == 1);
    }
    TEST_PASS("unblock-before-schedule race");

    // single task creation & execution

    // task runs and completes
    {
        counter_a = 0;
        sched_create_task(task_inc_a);
        sched_sleep_ms(50);
        TEST_ASSERT("single task ran", counter_a == 1);
    }
    TEST_PASS("single task");

    // task with loop (10 increments)
    {
        counter_a = 0;
        sched_create_task(task_inc_a_10x);
        sched_sleep_ms(50);
        TEST_ASSERT("loop task complete", counter_a == 10);
    }
    TEST_PASS("looping task");

    // multiple concurrent tasks

    // three distinct tasks
    {
        counter_a = 0;
        counter_b = 0;
        counter_c = 0;
        sched_create_task(task_inc_a);
        sched_create_task(task_inc_b);
        sched_create_task(task_inc_c);
        sched_sleep_ms(50);
        TEST_ASSERT("multi a ran", counter_a == 1);
        TEST_ASSERT("multi b ran", counter_b == 1);
        TEST_ASSERT("multi c ran", counter_c == 1);
    }
    TEST_PASS("three concurrent tasks");

    // same function spawned multiple times
    {
        counter_a = 0;
        sched_create_task(task_inc_a);
        sched_create_task(task_inc_a);
        sched_create_task(task_inc_a);
        sched_create_task(task_inc_a);
        sched_create_task(task_inc_a);
        sched_sleep_ms(50);
        TEST_ASSERT("5x same fn", counter_a == 5);
    }
    TEST_PASS("5x same function");

    // task execution order

    // fifo ordering: tasks enqueued in order run in order
    {
        order_idx = 0;
        for (int i = 0; i < 16; i++)
            order_log[i] = 0;
        sched_create_task(task_order_1);
        sched_create_task(task_order_2);
        sched_create_task(task_order_3);
        sched_sleep_ms(50);
        TEST_ASSERT("order count", order_idx == 3);
        // Round-robin FIFO: 1 enqueued first should run first
        TEST_ASSERT("order[0]==1", order_log[0] == 1);
        TEST_ASSERT("order[1]==2", order_log[1] == 2);
        TEST_ASSERT("order[2]==3", order_log[2] == 3);
    }
    TEST_PASS("FIFO task order");

    // sched_sleep_ms

    // basic sleep timing
    {
        unsigned long before = get_system_time();
        sched_sleep_ms(50);
        unsigned long after = get_system_time();
        unsigned long elapsed = after - before;
        // Timer has 100Hz tick (10ms granularity), allow generous bounds
        TEST_ASSERT("sleep >= 30ms", elapsed >= 30);
        TEST_ASSERT("sleep < 300ms", elapsed < 300);
    }
    TEST_PASS("sleep_ms basic timing");

    // short sleep
    {
        unsigned long before = get_system_time();
        sched_sleep_ms(20);
        unsigned long after = get_system_time();
        TEST_ASSERT("short sleep elapsed", (after - before) >= 10);
    }
    TEST_PASS("short sleep");

    // multiple sequential sleeps
    {
        unsigned long before = get_system_time();
        sched_sleep_ms(30);
        sched_sleep_ms(30);
        sched_sleep_ms(30);
        unsigned long after = get_system_time();
        unsigned long elapsed = after - before;
        TEST_ASSERT("3x30ms >= 60", elapsed >= 60);
    }
    TEST_PASS("sequential sleeps");

    // sleep ordering: short sleeper wakes before long sleeper
    {
        ts_short_done = 0;
        ts_long_done = 0;
        sched_create_task(task_long_sleep_ts);  // sleeps 40ms
        sched_create_task(task_short_sleep_ts); // sleeps 20ms
        sched_sleep_ms(80);
        TEST_ASSERT("short done", ts_short_done != 0);
        TEST_ASSERT("long done", ts_long_done != 0);
        TEST_ASSERT("short before long", ts_short_done < ts_long_done);
    }
    TEST_PASS("sleep queue ordering");

    // a task woken before its deadline must leave the sleep queue: otherwise
    // cleanup_dead_task frees it while sleep_drain still walks the list
    {
        early_wake_ran = 0;
        early_wake_task = NULL;
        sched_create_task(task_publish_then_long_sleep);
        sched_sleep_ms(30);

        unsigned long flags = spin_lock_irqsave(&test_lock);
        struct task *sleeper = early_wake_task;
        spin_unlock_irqrestore(&test_lock, flags);

        TEST_ASSERT("sleeper published itself", sleeper != NULL);
        TEST_ASSERT("sleeper is queued while sleeping", sched_test_in_sleep_queue(sleeper));

        // keep the wake and the check on one side of any preemption
        flags = irq_save();
        sched_unblock(sleeper);
        int still_queued = sched_test_in_sleep_queue(sleeper);
        irq_restore(flags);

        TEST_ASSERT("early wake unlinks from the sleep queue", !still_queued);

        sched_sleep_ms(30);
        TEST_ASSERT("woken sleeper ran to completion", early_wake_ran == 1);
    }
    TEST_PASS("early wake leaves the sleep queue");

    // task with sleep inside

    // task sleeps then increments
    {
        counter_a = 0;
        sched_create_task(task_sleep_then_inc);
        sched_sleep_ms(50);
        TEST_ASSERT("sleep-then-inc", counter_a == 1);
    }
    TEST_PASS("task with internal sleep");

    // task with multiple sleeps
    {
        counter_a = 0;
        sched_create_task(task_multi_sleep);
        sched_sleep_ms(80);
        TEST_ASSERT("multi-sleep task", counter_a == 3);
    }
    TEST_PASS("task with multiple sleeps");

    // task with interleaved work and sleep
    {
        counter_a = 0;
        sched_create_task(task_inc_a_with_delay);
        sched_sleep_ms(100);
        TEST_ASSERT("work+sleep task", counter_a == 5);
    }
    TEST_PASS("work+sleep interleaved");

    // concurrent sleepers

    // two tasks sleeping different durations
    {
        counter_a = 0;
        counter_b = 0;
        sched_create_task(task_sleep_then_inc);      // sleeps 20ms, inc a
        sched_create_task(task_long_sleep_then_inc); // sleeps 40ms, inc b
        sched_sleep_ms(30);
        TEST_ASSERT("short sleeper done", counter_a == 1);
        TEST_ASSERT("long sleeper not yet", counter_b == 0);
        sched_sleep_ms(50);
        TEST_ASSERT("long sleeper done", counter_b == 1);
    }
    TEST_PASS("concurrent sleepers");

    // task spawning tasks

    // parent task creates child tasks
    {
        counter_a = 0;
        sched_create_task(task_spawner); // creates 2 task_inc_a's
        sched_sleep_ms(100);
        TEST_ASSERT("spawned children ran", counter_a == 2);
    }
    TEST_PASS("task spawning children");

    // compute-bound task

    // task with no sleep completes via preemption
    {
        compute_result = 0;
        sched_create_task(task_compute);
        sched_sleep_ms(100);
        // sum of 0..9999 = 49995000
        TEST_ASSERT("compute result", compute_result == 49995000UL);
    }
    TEST_PASS("compute-bound task");

    // stack integrity

    // task using significant stack space
    {
        stack_test_ok = 0;
        sched_create_task(task_stack_depth);
        sched_sleep_ms(100);
        TEST_ASSERT("stack integrity", stack_test_ok == 1);
    }
    TEST_PASS("stack integrity");

    // rapid task creation

    // create and complete many tasks in quick succession
    //    This stresses dead-task cleanup (task_to_free path).
    {
        counter_a = 0;
        for (int i = 0; i < 8; i++)
            sched_create_task(task_inc_a);
        sched_sleep_ms(100);
        TEST_ASSERT("8 rapid tasks", counter_a == 8);
    }
    TEST_PASS("rapid 8 tasks");

    // batch-and-wait ×3 rounds
    {
        counter_a = 0;
        for (int round = 0; round < 3; round++) {
            for (int i = 0; i < 3; i++)
                sched_create_task(task_inc_a);
            sched_sleep_ms(50);
        }
        TEST_ASSERT("3 rounds of 3", counter_a == 9);
    }
    TEST_PASS("batch-and-wait 3x3");

    // shared data with mutual exclusion

    // two loop-tasks incrementing same counter
    {
        counter_a = 0;
        sched_create_task(task_inc_a_10x);
        sched_create_task(task_inc_a_10x);
        sched_sleep_ms(100);
        TEST_ASSERT("2x10 = 20", counter_a == 20);
    }
    TEST_PASS("concurrent increment 2x10");

    // mixed fast and slow tasks

    // fast instant task + slow sleeping task
    {
        counter_a = 0;
        counter_b = 0;
        sched_create_task(task_inc_a);               // instant
        sched_create_task(task_long_sleep_then_inc); // sleeps 40ms, inc b
        sched_sleep_ms(30);
        TEST_ASSERT("fast done", counter_a == 1);
        TEST_ASSERT("slow not yet", counter_b == 0);
        sched_sleep_ms(50);
        TEST_ASSERT("slow done", counter_b == 1);
    }
    TEST_PASS("mixed fast/slow tasks");

    // yield loop task
    {
        yield_count = 0;
        sched_create_task(task_yield_loop);
        sched_create_task(task_yield_loop);
        sched_sleep_ms(150);
        TEST_ASSERT("2x50 yields completed", yield_count == 100);
    }
    TEST_PASS("yield loop tasks");

    // system time monotonicity under scheduling

    // time always moves forward across sleeps
    {
        unsigned long t0 = get_system_time();
        sched_sleep_ms(20);
        unsigned long t1 = get_system_time();
        sched_sleep_ms(20);
        unsigned long t2 = get_system_time();
        sched_sleep_ms(20);
        unsigned long t3 = get_system_time();
        TEST_ASSERT("time monotonic t1>t0", t1 > t0);
        TEST_ASSERT("time monotonic t2>t1", t2 > t1);
        TEST_ASSERT("time monotonic t3>t2", t3 > t2);
    }
    TEST_PASS("time monotonicity");

    // lifecycle: complex sequence

    // create -> sleep -> create more -> wait for all
    {
        counter_a = 0;
        counter_b = 0;
        // Phase 1: two tasks
        sched_create_task(task_inc_a);
        sched_create_task(task_inc_b);
        sched_sleep_ms(50);
        TEST_ASSERT("phase1 a", counter_a == 1);
        TEST_ASSERT("phase1 b", counter_b == 1);

        // Phase 2: three more tasks
        sched_create_task(task_inc_a);
        sched_create_task(task_inc_a);
        sched_create_task(task_inc_b);
        sched_sleep_ms(50);
        TEST_ASSERT("phase2 a", counter_a == 3);
        TEST_ASSERT("phase2 b", counter_b == 2);
    }
    TEST_PASS("multi-phase lifecycle");

    TEST_SUITE_END("Scheduler");
}
