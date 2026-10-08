/*
 * test_rwsem.c - Tests for reader/writer sleeping semaphore (struct rwsem).
 */

#include "core/rwsem.h"

#include <stddef.h>

#include "core/lockdep.h"
#include "sched/sched.h"
#include "test.h"

#ifdef CONFIG_LOCKDEP
    #define EXPECT_LOCKDEP_DEPTH(name, n)                   \
        do {                                                \
            struct task *_t = sched_current_task();         \
            if (_t) {                                       \
                TEST_ASSERT_EQ(name, _t->lockdep_depth, n); \
            }                                               \
        } while (0)
#else
    #define EXPECT_LOCKDEP_DEPTH(name, n) \
        do {                              \
            (void)(name);                 \
            (void)(n);                    \
        } while (0)
#endif

void test_rwsem(void)
{
    TEST_SUITE_BEGIN("RWSEM");

    // 1. Init: rwsem_init and RWSEM_INIT match, and both queues are empty
    {
        struct rwsem s1 = RWSEM_INIT;
        struct rwsem s2;
        rwsem_init(&s2);

        TEST_ASSERT_EQ("rwsem: init readers == 0", s2.readers, 0);
        TEST_ASSERT("rwsem: init writer == NULL", s2.writer == NULL);
        TEST_ASSERT_EQ("rwsem: init writers_waiting == 0", s2.writers_waiting, 0);
        TEST_ASSERT("rwsem: init read_wq head is NULL", s2.read_wq.head == NULL);
        TEST_ASSERT("rwsem: init read_wq tail is NULL", s2.read_wq.tail == NULL);
        TEST_ASSERT("rwsem: init write_wq head is NULL", s2.write_wq.head == NULL);
        TEST_ASSERT("rwsem: init write_wq tail is NULL", s2.write_wq.tail == NULL);

        TEST_ASSERT_EQ("rwsem: static init matches readers", s1.readers, s2.readers);
        TEST_ASSERT("rwsem: static init matches writer", s1.writer == s2.writer);
        TEST_ASSERT_EQ("rwsem: static init matches writers_waiting", s1.writers_waiting,
                       s2.writers_waiting);
        TEST_ASSERT("rwsem: static init matches read_wq head", s1.read_wq.head == s2.read_wq.head);
        TEST_ASSERT("rwsem: static init matches write_wq head",
                    s1.write_wq.head == s2.write_wq.head);
    }

    // 2. Read: down_read gives readers == 1; up_read gives 0; lockdep depth 1 then 0
    {
        struct rwsem s = RWSEM_INIT;

        rwsem_down_read(&s);
        TEST_ASSERT_EQ("rwsem: down_read readers is 1", s.readers, 1);
        EXPECT_LOCKDEP_DEPTH("rwsem: lockdep depth is 1 while read held", 1);

        rwsem_up_read(&s);
        TEST_ASSERT_EQ("rwsem: up_read readers is 0", s.readers, 0);
        EXPECT_LOCKDEP_DEPTH("rwsem: lockdep depth is 0 after read release", 0);
    }

    // 3. Write: down_write gives writer == self and writers_waiting == 0; up_write gives NULL
    {
        struct rwsem s = RWSEM_INIT;
        struct task *self = sched_current_task();

        rwsem_down_write(&s);
        TEST_ASSERT("rwsem: down_write writer == self", s.writer == self);
        TEST_ASSERT_EQ("rwsem: down_write writers_waiting is 0", s.writers_waiting, 0);
        EXPECT_LOCKDEP_DEPTH("rwsem: lockdep depth is 1 while write held", 1);

        rwsem_up_write(&s);
        TEST_ASSERT("rwsem: up_write writer is NULL", s.writer == NULL);
        EXPECT_LOCKDEP_DEPTH("rwsem: lockdep depth is 0 after write release", 0);
    }

    // 4. Lockdep in quiet mode: recursive down_read gives 1 violation
#ifdef CONFIG_LOCKDEP
    {
        lockdep_test_quiet(1);
        int v0 = lockdep_test_violations();

        struct rwsem s_rec = RWSEM_INIT;
        rwsem_down_read(&s_rec);
        rwsem_down_read(&s_rec);

        TEST_ASSERT_EQ("rwsem: recursive down_read gives 1 lockdep violation",
                       lockdep_test_violations() - v0, 1);

        rwsem_up_read(&s_rec);
        rwsem_up_read(&s_rec);

        lockdep_test_quiet(0);
    }
#endif

    TEST_SUITE_END("RWSEM");
}

// 1. Readers share
static struct rwsem s_share = RWSEM_INIT;
static volatile int share_release;
static volatile int share_done[3];
static volatile int share_next_id;

static void task_share_reader(void)
{
    int id = __atomic_fetch_add(&share_next_id, 1, __ATOMIC_SEQ_CST);
    rwsem_down_read(&s_share);
    while (!share_release) {
        sched_sleep_ms(1);
    }
    rwsem_up_read(&s_share);
    share_done[id] = 1;
}

// 2. Writer keeps out a reader
static struct rwsem s_writer_blocks_reader = RWSEM_INIT;
static struct task *task_blocked_reader;
static volatile int blocked_reader_done;

static void task_single_reader(void)
{
    task_blocked_reader = sched_current_task();
    rwsem_down_read(&s_writer_blocks_reader);
    blocked_reader_done = 1;
    rwsem_up_read(&s_writer_blocks_reader);
}

// 3. Writer keeps out a writer
static struct rwsem s_writer_blocks_writer = RWSEM_INIT;
static struct task *task_blocked_writer;
static volatile int blocked_writer_done;

static void task_single_writer(void)
{
    task_blocked_writer = sched_current_task();
    rwsem_down_write(&s_writer_blocks_writer);
    blocked_writer_done = 1;
    rwsem_up_write(&s_writer_blocks_writer);
}

// 4. Readers keep out a writer
static struct rwsem s_readers_block_writer = RWSEM_INIT;
static volatile int hold_reader_release;
static volatile int hold_reader_done;
static struct task *task_reader_blocked_writer;
static volatile int reader_blocked_writer_done;

static void task_holding_reader(void)
{
    rwsem_down_read(&s_readers_block_writer);
    while (!hold_reader_release) {
        sched_sleep_ms(1);
    }
    rwsem_up_read(&s_readers_block_writer);
    hold_reader_done = 1;
}

static void task_waiting_writer(void)
{
    task_reader_blocked_writer = sched_current_task();
    rwsem_down_write(&s_readers_block_writer);
    reader_blocked_writer_done = 1;
    rwsem_up_write(&s_readers_block_writer);
}

// 5. Writer preference
static struct rwsem s_preference = RWSEM_INIT;
static struct task *task_pref_ra;
static struct task *task_pref_ww;
static struct task *task_pref_rb;
static volatile int pref_ra_release;
static volatile int pref_ww_release;
static volatile int pref_ra_done;
static volatile int pref_ww_done;
static volatile int pref_rb_done;
static volatile int pref_order_counter;
static volatile int pref_ww_order;
static volatile int pref_rb_order;

static void task_pref_reader_a(void)
{
    task_pref_ra = sched_current_task();
    rwsem_down_read(&s_preference);
    while (!pref_ra_release) {
        sched_sleep_ms(1);
    }
    rwsem_up_read(&s_preference);
    pref_ra_done = 1;
}

static void task_pref_writer_w(void)
{
    task_pref_ww = sched_current_task();
    rwsem_down_write(&s_preference);
    while (!pref_ww_release) {
        sched_sleep_ms(1);
    }
    pref_ww_order = __atomic_fetch_add(&pref_order_counter, 1, __ATOMIC_SEQ_CST);
    rwsem_up_write(&s_preference);
    pref_ww_done = 1;
}

static void task_pref_reader_b(void)
{
    task_pref_rb = sched_current_task();
    rwsem_down_read(&s_preference);
    pref_rb_order = __atomic_fetch_add(&pref_order_counter, 1, __ATOMIC_SEQ_CST);
    rwsem_up_read(&s_preference);
    pref_rb_done = 1;
}

// 6. Writer first when up_write has a choice
static struct rwsem s_choice = RWSEM_INIT;
static struct task *task_choice_h;
static struct task *task_choice_r;
static struct task *task_choice_w2;
static volatile int choice_h_release;
static volatile int choice_w2_release;
static volatile int choice_h_done;
static volatile int choice_r_done;
static volatile int choice_w2_done;
static volatile int choice_order_counter;
static volatile int choice_w2_order;
static volatile int choice_r_order;

static void task_choice_holder_h(void)
{
    task_choice_h = sched_current_task();
    rwsem_down_write(&s_choice);
    while (!choice_h_release) {
        sched_sleep_ms(1);
    }
    rwsem_up_write(&s_choice);
    choice_h_done = 1;
}

static void task_choice_reader_r(void)
{
    task_choice_r = sched_current_task();
    rwsem_down_read(&s_choice);
    choice_r_order = __atomic_fetch_add(&choice_order_counter, 1, __ATOMIC_SEQ_CST);
    rwsem_up_read(&s_choice);
    choice_r_done = 1;
}

static void task_choice_writer_w2(void)
{
    task_choice_w2 = sched_current_task();
    rwsem_down_write(&s_choice);
    while (!choice_w2_release) {
        sched_sleep_ms(1);
    }
    choice_w2_order = __atomic_fetch_add(&choice_order_counter, 1, __ATOMIC_SEQ_CST);
    rwsem_up_write(&s_choice);
    choice_w2_done = 1;
}

// 7. Stress test
#define STRESS_WORKERS 4
#define STRESS_ROUNDS  20

static struct rwsem s_stress = RWSEM_INIT;
static volatile int stress_next_id;
static volatile int stress_worker_done[STRESS_WORKERS];
static volatile int stress_cur_read;
static volatile int stress_cur_write;
static volatile int stress_violation_count;
static volatile int stress_peak_readers;

static void task_stress_worker(void)
{
    int id = __atomic_fetch_add(&stress_next_id, 1, __ATOMIC_SEQ_CST);

    for (int iter = 0; iter < STRESS_ROUNDS; iter++) {
        if ((id % 2) == 0) {
            // Even worker: reader
            rwsem_down_read(&s_stress);
            int readers_now = __atomic_fetch_add(&stress_cur_read, 1, __ATOMIC_SEQ_CST) + 1;
            int writers_now = __atomic_load_n(&stress_cur_write, __ATOMIC_SEQ_CST);

            if (writers_now > 0) {
                __atomic_fetch_add(&stress_violation_count, 1, __ATOMIC_SEQ_CST);
            }

            int prev_peak = __atomic_load_n(&stress_peak_readers, __ATOMIC_RELAXED);
            while (readers_now > prev_peak) {
                if (__atomic_compare_exchange_n(&stress_peak_readers, &prev_peak, readers_now, 0,
                                                __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                    break;
                }
            }

            sched_sleep_ms(1);

            __atomic_fetch_sub(&stress_cur_read, 1, __ATOMIC_SEQ_CST);
            rwsem_up_read(&s_stress);
        } else {
            // Odd worker: writer
            rwsem_down_write(&s_stress);
            int writers_now = __atomic_fetch_add(&stress_cur_write, 1, __ATOMIC_SEQ_CST) + 1;
            int readers_now = __atomic_load_n(&stress_cur_read, __ATOMIC_SEQ_CST);

            if (writers_now > 1 || (writers_now > 0 && readers_now > 0)) {
                __atomic_fetch_add(&stress_violation_count, 1, __ATOMIC_SEQ_CST);
            }

            sched_sleep_ms(1);

            __atomic_fetch_sub(&stress_cur_write, 1, __ATOMIC_SEQ_CST);
            rwsem_up_write(&s_stress);
        }
    }

    stress_worker_done[id] = 1;
}

void test_rwsem_scheduler(void)
{
    TEST_SUITE_BEGIN("RWSEM-MultiTask");

    // 1. Readers share
    {
        rwsem_init(&s_share);
        share_release = 0;
        share_next_id = 0;
        for (int i = 0; i < 3; i++) {
            share_done[i] = 0;
            sched_create_task(task_share_reader);
        }

        WAIT_UNTIL(s_share.readers == 3);
        TEST_ASSERT_EQ("rwsem: 3 readers overlap concurrently", s_share.readers, 3);

        share_release = 1;
        WAIT_UNTIL(share_done[0] && share_done[1] && share_done[2]);
        TEST_ASSERT("rwsem: all shared readers finished",
                    share_done[0] && share_done[1] && share_done[2]);
        TEST_ASSERT_EQ("rwsem: readers is 0 after shared readers finish", s_share.readers, 0);
        TEST_ASSERT("rwsem: read_wq empty after shared readers", s_share.read_wq.head == NULL);
        TEST_ASSERT("rwsem: write_wq empty after shared readers", s_share.write_wq.head == NULL);
    }

    // 2. Writer keeps out a reader
    {
        rwsem_init(&s_writer_blocks_reader);
        task_blocked_reader = NULL;
        blocked_reader_done = 0;

        rwsem_down_write(&s_writer_blocks_reader);
        sched_create_task(task_single_reader);

        WAIT_UNTIL(task_blocked(task_blocked_reader));
        TEST_ASSERT("rwsem: reader task is blocked by active writer",
                    task_blocked(task_blocked_reader));
        TEST_ASSERT_EQ("rwsem: reader done is 0 while writer holds lock", blocked_reader_done, 0);

        rwsem_up_write(&s_writer_blocks_reader);
        WAIT_UNTIL(blocked_reader_done);
        TEST_ASSERT("rwsem: reader finished after writer released", blocked_reader_done);
        TEST_ASSERT_EQ("rwsem: readers is 0 after single reader done",
                       s_writer_blocks_reader.readers, 0);
    }

    // 3. Writer keeps out a writer
    {
        rwsem_init(&s_writer_blocks_writer);
        task_blocked_writer = NULL;
        blocked_writer_done = 0;

        rwsem_down_write(&s_writer_blocks_writer);
        sched_create_task(task_single_writer);

        WAIT_UNTIL(task_blocked(task_blocked_writer));
        TEST_ASSERT("rwsem: second writer is blocked by first writer",
                    task_blocked(task_blocked_writer));
        TEST_ASSERT_EQ("rwsem: blocked writer done is 0 while first holds lock",
                       blocked_writer_done, 0);

        rwsem_up_write(&s_writer_blocks_writer);
        WAIT_UNTIL(blocked_writer_done);
        TEST_ASSERT("rwsem: second writer finished after first released", blocked_writer_done);
        TEST_ASSERT("rwsem: writer is NULL after second writer done",
                    s_writer_blocks_writer.writer == NULL);
    }

    // 4. Readers keep out a writer
    {
        rwsem_init(&s_readers_block_writer);
        hold_reader_release = 0;
        hold_reader_done = 0;
        task_reader_blocked_writer = NULL;
        reader_blocked_writer_done = 0;

        sched_create_task(task_holding_reader);
        WAIT_UNTIL(s_readers_block_writer.readers == 1);
        TEST_ASSERT_EQ("rwsem: holding reader acquired lock", s_readers_block_writer.readers, 1);

        sched_create_task(task_waiting_writer);
        WAIT_UNTIL(task_blocked(task_reader_blocked_writer));
        TEST_ASSERT("rwsem: writer blocked while reader holds lock",
                    task_blocked(task_reader_blocked_writer));
        TEST_ASSERT_EQ("rwsem: blocked writer not finished", reader_blocked_writer_done, 0);

        hold_reader_release = 1;
        WAIT_UNTIL(hold_reader_done && reader_blocked_writer_done);
        TEST_ASSERT("rwsem: writer finished after holding reader released",
                    reader_blocked_writer_done);
        TEST_ASSERT_EQ("rwsem: readers is 0 after writer completes", s_readers_block_writer.readers,
                       0);
        TEST_ASSERT("rwsem: writer is NULL after completion",
                    s_readers_block_writer.writer == NULL);
    }

    // 5. Writer preference
    {
        rwsem_init(&s_preference);
        task_pref_ra = NULL;
        task_pref_ww = NULL;
        task_pref_rb = NULL;
        pref_ra_release = 0;
        pref_ww_release = 0;
        pref_ra_done = 0;
        pref_ww_done = 0;
        pref_rb_done = 0;
        pref_order_counter = 0;
        pref_ww_order = -1;
        pref_rb_order = -1;

        // Reader A takes lock
        sched_create_task(task_pref_reader_a);
        WAIT_UNTIL(s_preference.readers == 1);
        TEST_ASSERT_EQ("rwsem: pref: reader A holds lock", s_preference.readers, 1);

        // Writer W arrives and blocks
        sched_create_task(task_pref_writer_w);
        WAIT_UNTIL(task_blocked(task_pref_ww));
        TEST_ASSERT("rwsem: pref: writer W is blocked", task_blocked(task_pref_ww));
        TEST_ASSERT_EQ("rwsem: pref: writers_waiting is 1", s_preference.writers_waiting, 1);

        // Reader B arrives and must block too because writer is waiting
        sched_create_task(task_pref_reader_b);
        WAIT_UNTIL(task_blocked(task_pref_rb));
        TEST_ASSERT("rwsem: pref: reader B blocked behind waiting writer",
                    task_blocked(task_pref_rb));
        TEST_ASSERT_EQ("rwsem: pref: readers still 1 while B blocked", s_preference.readers, 1);

        // Release reader A: W gets lock, B stays blocked
        pref_ra_release = 1;
        WAIT_UNTIL(s_preference.writer == task_pref_ww);
        TEST_ASSERT("rwsem: pref: writer W got lock after reader A released",
                    s_preference.writer == task_pref_ww);
        TEST_ASSERT("rwsem: pref: reader B still blocked while W holds lock",
                    task_blocked(task_pref_rb));

        // Release writer W: reader B gets lock and finishes
        pref_ww_release = 1;
        WAIT_UNTIL(pref_ra_done && pref_ww_done && pref_rb_done);
        TEST_ASSERT("rwsem: pref: all tasks finished",
                    pref_ra_done && pref_ww_done && pref_rb_done);
        TEST_ASSERT("rwsem: pref: writer finished before reader B", pref_ww_order < pref_rb_order);
        TEST_ASSERT_EQ("rwsem: pref: writer finish order was 0", pref_ww_order, 0);
        TEST_ASSERT_EQ("rwsem: pref: reader B finish order was 1", pref_rb_order, 1);
    }

    // 6. Writer first when up_write has a choice
    {
        rwsem_init(&s_choice);
        task_choice_h = NULL;
        task_choice_r = NULL;
        task_choice_w2 = NULL;
        choice_h_release = 0;
        choice_w2_release = 0;
        choice_h_done = 0;
        choice_r_done = 0;
        choice_w2_done = 0;
        choice_order_counter = 0;
        choice_w2_order = -1;
        choice_r_order = -1;

        // Holder H takes write lock
        sched_create_task(task_choice_holder_h);
        WAIT_UNTIL(s_choice.writer == task_choice_h);
        TEST_ASSERT("rwsem: choice: holder H has write lock", s_choice.writer == task_choice_h);

        // Reader R blocks
        sched_create_task(task_choice_reader_r);
        WAIT_UNTIL(task_blocked(task_choice_r));
        TEST_ASSERT("rwsem: choice: reader R is blocked", task_blocked(task_choice_r));

        // Writer W2 blocks
        sched_create_task(task_choice_writer_w2);
        WAIT_UNTIL(task_blocked(task_choice_w2));
        TEST_ASSERT("rwsem: choice: writer W2 is blocked", task_blocked(task_choice_w2));
        TEST_ASSERT_EQ("rwsem: choice: writers_waiting is 1", s_choice.writers_waiting, 1);

        // Release H: W2 must run first, R stays blocked
        choice_h_release = 1;
        WAIT_UNTIL(s_choice.writer == task_choice_w2);
        TEST_ASSERT("rwsem: choice: W2 was chosen over R", s_choice.writer == task_choice_w2);
        TEST_ASSERT("rwsem: choice: R is still blocked while W2 holds lock",
                    task_blocked(task_choice_r));

        // Release W2: R now gets lock and finishes
        choice_w2_release = 1;
        WAIT_UNTIL(choice_h_done && choice_w2_done && choice_r_done);
        TEST_ASSERT("rwsem: choice: all tasks finished",
                    choice_h_done && choice_w2_done && choice_r_done);
        TEST_ASSERT("rwsem: choice: W2 finished before R", choice_w2_order < choice_r_order);
        TEST_ASSERT_EQ("rwsem: choice: W2 order was 0", choice_w2_order, 0);
        TEST_ASSERT_EQ("rwsem: choice: R order was 1", choice_r_order, 1);
    }

    // 7. Stress test
    {
        rwsem_init(&s_stress);
        stress_next_id = 0;
        stress_cur_read = 0;
        stress_cur_write = 0;
        stress_violation_count = 0;
        stress_peak_readers = 0;

        for (int i = 0; i < STRESS_WORKERS; i++) {
            stress_worker_done[i] = 0;
            sched_create_task(task_stress_worker);
        }

        WAIT_UNTIL(stress_worker_done[0] && stress_worker_done[1] && stress_worker_done[2]
                   && stress_worker_done[3]);
        TEST_ASSERT("rwsem: stress: all 4 workers finished",
                    stress_worker_done[0] && stress_worker_done[1] && stress_worker_done[2]
                        && stress_worker_done[3]);

        TEST_ASSERT_EQ("rwsem: stress: zero mutual exclusion violations", stress_violation_count,
                       0);
        TEST_ASSERT_EQ("rwsem: stress: readers is 0 at end", s_stress.readers, 0);
        TEST_ASSERT("rwsem: stress: writer is NULL at end", s_stress.writer == NULL);
        TEST_ASSERT_EQ("rwsem: stress: writers_waiting is 0 at end", s_stress.writers_waiting, 0);
        TEST_ASSERT("rwsem: stress: read_wq is empty at end", s_stress.read_wq.head == NULL);
        TEST_ASSERT("rwsem: stress: write_wq is empty at end", s_stress.write_wq.head == NULL);
        TEST_ASSERT("rwsem: stress: peak concurrent readers was >= 2", stress_peak_readers >= 2);
    }

    TEST_SUITE_END("RWSEM-MultiTask");
}
