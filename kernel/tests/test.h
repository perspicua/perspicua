/*
 * test.h - Assertion macros, suite bookkeeping and the helpers the self-test suites share.
 */

#ifndef PERSPICUA_TESTS_TEST_H
#define PERSPICUA_TESTS_TEST_H

#include <stdint.h>

#include "sched/sched.h"
#include "stdio.h"

extern int tests_passed;
extern int tests_failed;
extern int test_suite_failed;
extern int test_suite_passed;

// Atomic so an assert from a spawned task on another core is never lost.
#define TEST_COUNT(counter) __atomic_fetch_add(&(counter), 1, __ATOMIC_RELAXED)

// A group of n asserts the environment cannot run: logged, and counted so the suite's total holds.
#define TEST_SKIP(reason, n)                                         \
    do {                                                             \
        pr_info("test: %s; %d asserts skipped\n", reason, (int)(n)); \
        __atomic_fetch_add(&tests_passed, (n), __ATOMIC_RELAXED);    \
    } while (0)

#define TEST_ASSERT(name, cond)                             \
    do {                                                    \
        if (!(cond)) {                                      \
            pr_err("test: %s: %s [FAILED]\n", name, #cond); \
            TEST_COUNT(tests_failed);                       \
        } else {                                            \
            TEST_COUNT(tests_passed);                       \
        }                                                   \
    } while (0)

#define TEST_ASSERT_EQ(name, actual, expected)                                  \
    do {                                                                        \
        long _a = (long)(actual);                                               \
        long _e = (long)(expected);                                             \
        if (_a != _e) {                                                         \
            pr_err("test: %s: expected %ld, got %ld [FAILED]\n", name, _e, _a); \
            TEST_COUNT(tests_failed);                                           \
        } else {                                                                \
            TEST_COUNT(tests_passed);                                           \
        }                                                                       \
    } while (0)

#define TEST_SUITE_BEGIN(name)            \
    do {                                  \
        test_suite_failed = tests_failed; \
        test_suite_passed = tests_passed; \
    } while (0)

// The count lets run_tests.sh notice a suite that quietly stopped running some of its tests.
#define TEST_SUITE_END(name)                                                                    \
    do {                                                                                        \
        if (tests_failed == test_suite_failed)                                                  \
            pr_info("test: %s: %d tests [OK]\n", name, tests_passed - test_suite_passed);       \
        else                                                                                    \
            pr_err("test: %s: %d failures [FAILED]\n", name, tests_failed - test_suite_failed); \
    } while (0)

// Polls for up to about two seconds; the asserts that follow report what was reached.
#define WAIT_UNTIL(cond)                              \
    do {                                              \
        for (int _n = 0; _n < 200 && !(cond); _n++) { \
            sched_sleep_ms(5);                        \
        }                                             \
    } while (0)

// True once a spawned task has reached its blocking point.
static inline int task_blocked(struct task *t)
{
    return t && __atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == SCHED_TASK_BLOCKED;
}

// Bookkeeping for a set of spawned waiters: tasks[i] is the waiter, done[i] set when it finished.
static inline int waiters_all_blocked(struct task *const *tasks, int n)
{
    for (int i = 0; i < n; i++) {
        if (!task_blocked(tasks[i])) {
            return 0;
        }
    }
    return 1;
}

static inline int waiters_count_done(const volatile int *done, int n)
{
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (done[i]) {
            count++;
        }
    }
    return count;
}

static inline int waiters_all_done(const volatile int *done, int n)
{
    return waiters_count_done(done, n) == n;
}

static inline int waiters_count_blocked(struct task *const *tasks, const volatile int *done, int n)
{
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (!done[i] && task_blocked(tasks[i])) {
            count++;
        }
    }
    return count;
}

// SIGUSR1 pending on slot 0, which an interruptible wait of a kernel task reports as -ERESTARTSYS.
void test_sigusr1_pend(void);
void test_sigusr1_clear(void);

// What a signal does to a sleeping task: pends SIGUSR1, then wakes it to notice.
void test_sigusr1_interrupt(struct task *t);

void run_all_tests(void);

// Issues a syscall from the running task, as though trapped from EL0.
int64_t test_syscall(uint64_t nr, const uint64_t *args);

// Lends slot 0 an empty address space as TTBR0; test_release_user_pgd frees it and its pages.
unsigned long *test_borrow_user_pgd(void);
void test_release_user_pgd(unsigned long *pgd);

// Maps a fresh zeroed page at va. NULL leaves nothing allocated.
void *test_map_user_page(unsigned long *pgd, unsigned long va, unsigned long flags);

// lib tests
void test_string(void);
void test_ksym(void);

// kernel tests
void test_spinlock(void);
void test_pmm(void);
void test_slab(void);
void test_heap(void);
void test_heap_props(void);
void test_timer(void);
void test_sd(void);
void test_mailbox(void);
void test_mmu(void);
void test_mmu_user(void);
void test_vfs(void);
void test_fat32(void);
void test_fat32_corrupt(void);
void test_pipe(void);
void test_procfs(void);
void test_mutex(void);
void test_semaphore(void);
void test_completion(void);
void test_rwsem(void);
void test_wait(void);
void test_uaccess(void);
void test_process(void);
void test_elf(void);
void test_syscall_bounds(void);
void test_faultinject(void);
void test_scheduler(void);
void test_wait_scheduler(void);
void test_semaphore_scheduler(void);
void test_completion_scheduler(void);
void test_rwsem_scheduler(void);
void test_spinlock_scheduler(void);
void test_tty_scheduler(void);
#ifdef CONFIG_LOCKDEP
void test_lockdep_scheduler(void);
void test_lockdep_spin(void);
#else
static inline void test_lockdep_scheduler(void) {}
static inline void test_lockdep_spin(void) {}
#endif

// scheduler tests (must be called after enable_interrupts + sched_init)
void run_scheduler_tests(void);

// post-init tests (must be called after a user process exists)
void run_post_init_tests(void);
void test_signals(void);

#endif // PERSPICUA_TESTS_TEST_H
