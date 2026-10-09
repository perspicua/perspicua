/*
 * test_lockdep.c - Tests for sleeping lockdep validator.
 */

#ifdef CONFIG_LOCKDEP

    #include "core/lockdep.h"
    #include "core/completion.h"
    #include "core/lock.h"
    #include "core/mutex.h"
    #include "core/semaphore.h"
    #include "sched/sched.h"
    #include "test.h"

static int la, lb, lc;

static struct kmutex km_rec;
static struct kmutex km_a;
static struct kmutex km_b;
static struct kmutex km_spl;
static struct kmutex km_exit;
static struct ksem sem_spl;
static struct completion comp_spl;

static void task_exit_holding_lock(void)
{
    kmutex_lock(&km_exit);
}

void test_lockdep_scheduler(void)
{
    TEST_SUITE_BEGIN("Lockdep-Sleeping");

    // Case a: Acquire and release A: depth goes 1 then 0, and held[0] == &la. No violations.
    {
        lockdep_test_quiet(1);
        int v0 = lockdep_test_violations();
        struct task *t = sched_current_task();

        lockdep_acquire_sleep(&la);
        TEST_ASSERT_EQ("lockdep: acquire A depth is 1", t->lockdep_depth, 1);
        TEST_ASSERT("lockdep: acquire A held[0] is &la", t->lockdep_held[0] == &la);

        lockdep_release_sleep(&la);
        TEST_ASSERT_EQ("lockdep: release A depth is 0", t->lockdep_depth, 0);

        TEST_ASSERT_EQ("lockdep: case a violations", lockdep_test_violations() - v0, 0);
        lockdep_test_quiet(0);
    }

    // Case b: Normal nesting: A then B, released B then A, gives 0 violations.
    // Then A then B, released A then B, gives 0 violations and the depth stays correct.
    {
        lockdep_test_quiet(1);
        int v0 = lockdep_test_violations();
        struct task *t = sched_current_task();

        // Part 1: A then B, released B then A
        lockdep_acquire_sleep(&la);
        TEST_ASSERT_EQ("lockdep: nest acquire A depth is 1", t->lockdep_depth, 1);
        TEST_ASSERT("lockdep: nest acquire A held[0]", t->lockdep_held[0] == &la);

        lockdep_acquire_sleep(&lb);
        TEST_ASSERT_EQ("lockdep: nest acquire B depth is 2", t->lockdep_depth, 2);
        TEST_ASSERT("lockdep: nest acquire B held[1]", t->lockdep_held[1] == &lb);

        lockdep_release_sleep(&lb);
        TEST_ASSERT_EQ("lockdep: nest release B depth is 1", t->lockdep_depth, 1);
        TEST_ASSERT("lockdep: nest release B leaves A held", t->lockdep_held[0] == &la);

        lockdep_release_sleep(&la);
        TEST_ASSERT_EQ("lockdep: nest release A depth is 0", t->lockdep_depth, 0);

        TEST_ASSERT_EQ("lockdep: nest LIFO violations", lockdep_test_violations() - v0, 0);

        // Part 2: A then B, released A then B (out of order release)
        int v1 = lockdep_test_violations();
        lockdep_acquire_sleep(&la);
        TEST_ASSERT_EQ("lockdep: ooo acquire A depth is 1", t->lockdep_depth, 1);
        lockdep_acquire_sleep(&lb);
        TEST_ASSERT_EQ("lockdep: ooo acquire B depth is 2", t->lockdep_depth, 2);

        lockdep_release_sleep(&la);
        TEST_ASSERT_EQ("lockdep: ooo release A depth is 1", t->lockdep_depth, 1);
        TEST_ASSERT("lockdep: ooo release A leaves B at held[0]", t->lockdep_held[0] == &lb);

        lockdep_release_sleep(&lb);
        TEST_ASSERT_EQ("lockdep: ooo release B depth is 0", t->lockdep_depth, 0);

        TEST_ASSERT_EQ("lockdep: nest FIFO violations", lockdep_test_violations() - v1, 0);
        lockdep_test_quiet(0);
    }

    // Case c: Reverse order: B then A gives 1 violation, because case b already recorded A before B.
    {
        lockdep_test_quiet(1);
        int v0 = lockdep_test_violations();
        struct task *t = sched_current_task();

        lockdep_acquire_sleep(&lb);
        TEST_ASSERT_EQ("lockdep: reverse acquire B depth 1", t->lockdep_depth, 1);

        lockdep_acquire_sleep(&la);
        TEST_ASSERT_EQ("lockdep: reverse acquire A depth 2", t->lockdep_depth, 2);
        TEST_ASSERT_EQ("lockdep: reverse order gave 1 violation", lockdep_test_violations() - v0,
                       1);

        lockdep_release_sleep(&la);
        lockdep_release_sleep(&lb);
        TEST_ASSERT_EQ("lockdep: reverse order cleanup depth 0", t->lockdep_depth, 0);

        lockdep_test_quiet(0);
    }

    // Case d: Transitive: take B then C, so B is before C.
    // Then C then A gives 1 violation, because A is before B and B is before C.
    {
        lockdep_test_quiet(1);
        int v0 = lockdep_test_violations();
        struct task *t = sched_current_task();

        // 1. Take B then C
        lockdep_acquire_sleep(&lb);
        lockdep_acquire_sleep(&lc);
        TEST_ASSERT_EQ("lockdep: transitive setup violations", lockdep_test_violations() - v0, 0);

        lockdep_release_sleep(&lc);
        lockdep_release_sleep(&lb);
        TEST_ASSERT_EQ("lockdep: transitive setup cleanup depth 0", t->lockdep_depth, 0);

        // 2. Take C then A: cycle because A is before B and B is before C
        int v1 = lockdep_test_violations();
        lockdep_acquire_sleep(&lc);
        lockdep_acquire_sleep(&la);
        TEST_ASSERT_EQ("lockdep: transitive cycle gave 1 violation", lockdep_test_violations() - v1,
                       1);

        lockdep_release_sleep(&la);
        lockdep_release_sleep(&lc);
        TEST_ASSERT_EQ("lockdep: transitive cleanup depth 0", t->lockdep_depth, 0);

        lockdep_test_quiet(0);
    }

    // Case e: Recursion: A then A gives 1 violation.
    {
        lockdep_test_quiet(1);
        int v0 = lockdep_test_violations();
        struct task *t = sched_current_task();

        lockdep_acquire_sleep(&la);
        TEST_ASSERT_EQ("lockdep: recursion acquire A depth 1", t->lockdep_depth, 1);

        lockdep_acquire_sleep(&la);
        TEST_ASSERT_EQ("lockdep: recursion acquire A again depth 2", t->lockdep_depth, 2);
        TEST_ASSERT_EQ("lockdep: recursion gave 1 violation", lockdep_test_violations() - v0, 1);

        lockdep_release_sleep(&la);
        TEST_ASSERT_EQ("lockdep: recursion release 1 depth 1", t->lockdep_depth, 1);
        lockdep_release_sleep(&la);
        TEST_ASSERT_EQ("lockdep: recursion release 2 depth 0", t->lockdep_depth, 0);

        lockdep_test_quiet(0);
    }

    // Case f: might_sleep under a spinlock: take a real stack spinlock,
    // call lockdep_might_sleep(), release spinlock, gives 1 violation.
    {
        lockdep_test_quiet(1);
        int v0 = lockdep_test_violations();

        spinlock_t spl = SPINLOCK_INIT;
        spin_lock(&spl);
        lockdep_might_sleep();
        spin_unlock(&spl);

        TEST_ASSERT_EQ("lockdep: might_sleep under spinlock gave 1 violation",
                       lockdep_test_violations() - v0, 1);

        lockdep_test_quiet(0);
    }

    // Case g: assert_no_sleep_locks: with A held it's 1 violation; with nothing held it's 0.
    {
        lockdep_test_quiet(1);
        int v0 = lockdep_test_violations();
        struct task *t = sched_current_task();

        // 1. With A held
        lockdep_acquire_sleep(&la);
        TEST_ASSERT_EQ("lockdep: assert_no_sleep_locks depth 1", t->lockdep_depth, 1);

        lockdep_assert_no_sleep_locks("exit");
        TEST_ASSERT_EQ("lockdep: assert_no_sleep_locks with held gave 1 violation",
                       lockdep_test_violations() - v0, 1);

        lockdep_release_sleep(&la);
        TEST_ASSERT_EQ("lockdep: assert_no_sleep_locks depth 0", t->lockdep_depth, 0);

        // 2. With nothing held
        int v1 = lockdep_test_violations();
        lockdep_assert_no_sleep_locks("exit");
        TEST_ASSERT_EQ("lockdep: assert_no_sleep_locks with empty gave 0 violations",
                       lockdep_test_violations() - v1, 0);

        lockdep_test_quiet(0);
    }

    // Real primitive tests (Step 3b):

    // 1. Recursion is invisible to lockdep for kmutex
    {
        lockdep_test_quiet(1);
        int v0 = lockdep_test_violations();
        struct task *t = sched_current_task();

        kmutex_init(&km_rec);
        kmutex_lock(&km_rec);
        TEST_ASSERT_EQ("lockdep: kmutex first lock depth 1", t->lockdep_depth, 1);
        TEST_ASSERT("lockdep: kmutex first lock held[0] == &km_rec", t->lockdep_held[0] == &km_rec);

        kmutex_lock(&km_rec);
        TEST_ASSERT_EQ("lockdep: kmutex recursive lock depth 1", t->lockdep_depth, 1);
        TEST_ASSERT("lockdep: kmutex recursive lock held[0] == &km_rec",
                    t->lockdep_held[0] == &km_rec);
        TEST_ASSERT_EQ("lockdep: kmutex recursive lock violations 0",
                       lockdep_test_violations() - v0, 0);

        kmutex_unlock(&km_rec);
        TEST_ASSERT_EQ("lockdep: kmutex first unlock depth still 1", t->lockdep_depth, 1);
        TEST_ASSERT("lockdep: kmutex first unlock held[0] == &km_rec",
                    t->lockdep_held[0] == &km_rec);

        kmutex_unlock(&km_rec);
        TEST_ASSERT_EQ("lockdep: kmutex second unlock depth 0", t->lockdep_depth, 0);
        TEST_ASSERT_EQ("lockdep: kmutex second unlock violations 0", lockdep_test_violations() - v0,
                       0);

        lockdep_test_quiet(0);
    }

    // 2. kmutex order: ma then mb is fine. Then mb then ma gives 1 violation.
    {
        lockdep_test_quiet(1);
        int v0 = lockdep_test_violations();

        kmutex_init(&km_a);
        kmutex_init(&km_b);

        kmutex_lock(&km_a);
        kmutex_lock(&km_b);
        TEST_ASSERT_EQ("lockdep: kmutex order A then B violations 0",
                       lockdep_test_violations() - v0, 0);
        kmutex_unlock(&km_b);
        kmutex_unlock(&km_a);

        int v1 = lockdep_test_violations();
        kmutex_lock(&km_b);
        kmutex_lock(&km_a);
        TEST_ASSERT_EQ("lockdep: kmutex order B then A gives 1 violation",
                       lockdep_test_violations() - v1, 1);
        kmutex_unlock(&km_a);
        kmutex_unlock(&km_b);

        lockdep_test_quiet(0);
    }

    // 3. kmutex under a spinlock: 1 violation
    {
        lockdep_test_quiet(1);
        int v0 = lockdep_test_violations();

        kmutex_init(&km_spl);
        spinlock_t spl = SPINLOCK_INIT;
        spin_lock(&spl);
        kmutex_lock(&km_spl);
        spin_unlock(&spl);

        TEST_ASSERT_EQ("lockdep: kmutex under spinlock gives 1 violation",
                       lockdep_test_violations() - v0, 1);

        kmutex_unlock(&km_spl);
        lockdep_test_quiet(0);
    }

    // 4. Fast path is checked too:
    // - ksem_down under a spinlock with a unit free gives 1 violation
    // - completion_wait under a spinlock on an already done completion gives 1 violation
    {
        lockdep_test_quiet(1);
        int v0 = lockdep_test_violations();

        ksem_init(&sem_spl, 1);
        spinlock_t spl1 = SPINLOCK_INIT;
        spin_lock(&spl1);
        ksem_down(&sem_spl);
        spin_unlock(&spl1);

        TEST_ASSERT_EQ("lockdep: ksem_down under spinlock gives 1 violation",
                       lockdep_test_violations() - v0, 1);

        int v1 = lockdep_test_violations();
        completion_init(&comp_spl);
        complete(&comp_spl);

        spinlock_t spl2 = SPINLOCK_INIT;
        spin_lock(&spl2);
        completion_wait(&comp_spl);
        spin_unlock(&spl2);

        TEST_ASSERT_EQ("lockdep: completion_wait under spinlock gives 1 violation",
                       lockdep_test_violations() - v1, 1);

        lockdep_test_quiet(0);
    }

    // 5. Task exits holding a lock: gives 1 violation
    {
        kmutex_init(&km_exit);

        lockdep_test_quiet(1);
        int v0 = lockdep_test_violations();

        sched_create_task(task_exit_holding_lock);

        WAIT_UNTIL(lockdep_test_violations() - v0 == 1);
        TEST_ASSERT_EQ("lockdep: task exit holding lock gives 1 violation",
                       lockdep_test_violations() - v0, 1);

        lockdep_test_quiet(0);
    }

    // 6. lockdep_assert_preemptible checks:
    // (1) Under one spinlock: 1 violation
    {
        lockdep_test_quiet(1);
        int v0 = lockdep_test_violations();

        spinlock_t spl = SPINLOCK_INIT;
        spin_lock(&spl);
        lockdep_assert_preemptible();
        spin_unlock(&spl);

        TEST_ASSERT_EQ("lockdep: assert_preemptible under spinlock gives 1 violation",
                       lockdep_test_violations() - v0, 1);
        lockdep_test_quiet(0);
    }

    // (2) Under spin_lock_irqsave: 1 violation
    {
        lockdep_test_quiet(1);
        int v0 = lockdep_test_violations();

        spinlock_t spl = SPINLOCK_INIT;
        unsigned long flags = spin_lock_irqsave(&spl);
        lockdep_assert_preemptible();
        spin_unlock_irqrestore(&spl, flags);

        TEST_ASSERT_EQ("lockdep: assert_preemptible under spin_lock_irqsave gives 1 violation",
                       lockdep_test_violations() - v0, 1);
        lockdep_test_quiet(0);
    }

    // (3) Nothing held: 0 violations
    {
        lockdep_test_quiet(1);
        int v0 = lockdep_test_violations();

        lockdep_assert_preemptible();

        TEST_ASSERT_EQ("lockdep: assert_preemptible with nothing held gives 0 violations",
                       lockdep_test_violations() - v0, 0);
        lockdep_test_quiet(0);
    }

    // Case h: At the end of the suite: lockdep_depth == 0.
    {
        struct task *t = sched_current_task();
        TEST_ASSERT_EQ("lockdep: depth is 0 at end of suite", t->lockdep_depth, 0);
    }

    TEST_SUITE_END("Lockdep-Sleeping");
}

#endif // CONFIG_LOCKDEP
