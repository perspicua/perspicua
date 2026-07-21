/*
 * test_mutex.c - Tests for the recursive sleeping mutex (kmutex).
 *
 * Only the uncontended and recursive paths are covered here; the blocking
 * path needs a second runnable task and belongs in the scheduler phase.
 */

#include "test.h"

#include "core/mutex.h"
#include "sched/sched.h"

void test_mutex(void)
{
    TEST_SUITE_BEGIN("Mutex");

    struct task *self = sched_get_current();

    // initialisation clears ownership
    {
        struct kmutex m;
        kmutex_init(&m);
        TEST_ASSERT("init leaves unowned", m.owner == NULL);
        TEST_ASSERT_EQ("init leaves depth 0", (long)m.depth, 0);
        TEST_ASSERT("init leaves empty wait queue", m.wait_head == NULL);
        TEST_ASSERT("init leaves null wait tail", m.wait_tail == NULL);
    }
    TEST_PASS("init");

    // the static initialiser must match kmutex_init
    {
        struct kmutex statically = KMUTEX_INIT;
        TEST_ASSERT("static init unowned", statically.owner == NULL);
        TEST_ASSERT_EQ("static init depth 0", (long)statically.depth, 0);
        TEST_ASSERT("static init empty queue", statically.wait_head == NULL);
    }
    TEST_PASS("static initialiser");

    // uncontended lock takes ownership and unlock releases it
    {
        struct kmutex m;
        kmutex_init(&m);

        kmutex_lock(&m);
        TEST_ASSERT("lock records owner", m.owner == self);
        TEST_ASSERT_EQ("lock sets depth 1", (long)m.depth, 1);

        kmutex_unlock(&m);
        TEST_ASSERT("unlock clears owner", m.owner == NULL);
        TEST_ASSERT_EQ("unlock clears depth", (long)m.depth, 0);
    }
    TEST_PASS("lock/unlock");

    /*
     * Recursion is the whole point of kmutex over a plain sleeping lock: the
     * owner must be able to re-enter without deadlocking, and ownership must
     * survive until the final matching unlock.
     */
    {
        struct kmutex m;
        kmutex_init(&m);

        kmutex_lock(&m);
        kmutex_lock(&m);
        kmutex_lock(&m);
        TEST_ASSERT_EQ("three locks nest to depth 3", (long)m.depth, 3);
        TEST_ASSERT("owner unchanged while nested", m.owner == self);

        kmutex_unlock(&m);
        TEST_ASSERT_EQ("depth drops to 2", (long)m.depth, 2);
        TEST_ASSERT("still owned at depth 2", m.owner == self);

        kmutex_unlock(&m);
        TEST_ASSERT_EQ("depth drops to 1", (long)m.depth, 1);
        TEST_ASSERT("still owned at depth 1", m.owner == self);

        kmutex_unlock(&m);
        TEST_ASSERT_EQ("final unlock clears depth", (long)m.depth, 0);
        TEST_ASSERT("final unlock releases ownership", m.owner == NULL);
    }
    TEST_PASS("recursive locking");

    // relocking after a full release must start from a clean depth
    {
        struct kmutex m;
        kmutex_init(&m);

        kmutex_lock(&m);
        kmutex_unlock(&m);
        kmutex_lock(&m);
        TEST_ASSERT_EQ("relock starts at depth 1", (long)m.depth, 1);
        TEST_ASSERT("relock records owner", m.owner == self);
        kmutex_unlock(&m);
        TEST_ASSERT_EQ("relock released", (long)m.depth, 0);
    }
    TEST_PASS("relock after release");

    // independent mutexes must not share state
    {
        struct kmutex a, b;
        kmutex_init(&a);
        kmutex_init(&b);

        kmutex_lock(&a);
        TEST_ASSERT("locking a leaves b free", b.owner == NULL);
        kmutex_lock(&b);
        TEST_ASSERT("both held independently", a.owner == self && b.owner == self);

        kmutex_unlock(&b);
        TEST_ASSERT("unlocking b leaves a held", a.owner == self);
        kmutex_unlock(&a);
        TEST_ASSERT("both released", a.owner == NULL && b.owner == NULL);
    }
    TEST_PASS("independent mutexes");

    TEST_SUITE_END("Mutex");
}
