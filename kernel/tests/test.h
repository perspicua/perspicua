#ifndef _TEST_H_
#define _TEST_H_

#include "stdio.h"

extern int tests_passed;
extern int tests_failed;
extern int _suite_failed;

// test assertion macros

#define TEST_ASSERT(name, cond)                             \
    do {                                                    \
        if (!(cond)) {                                      \
            pr_err("test: %s: %s [FAILED]\n", name, #cond); \
            tests_failed++;                                 \
            return;                                         \
        }                                                   \
    } while (0)

#define TEST_ASSERT_EQ(name, actual, expected)                                  \
    do {                                                                        \
        long _a = (long)(actual);                                               \
        long _e = (long)(expected);                                             \
        if (_a != _e) {                                                         \
            pr_err("test: %s: expected %ld, got %ld [FAILED]\n", name, _e, _a); \
            tests_failed++;                                                     \
            return;                                                             \
        }                                                                       \
    } while (0)

#define TEST_ASSERT_NEQ(name, actual, unexpected)                          \
    do {                                                                   \
        long _a = (long)(actual);                                          \
        long _u = (long)(unexpected);                                      \
        if (_a == _u) {                                                    \
            pr_err("test: %s: unexpected value %ld [FAILED]\n", name, _u); \
            tests_failed++;                                                \
            return;                                                        \
        }                                                                  \
    } while (0)

#define TEST_PASS(name) \
    do {                \
        tests_passed++; \
    } while (0)

#define TEST_SUITE_BEGIN(name)        \
    do {                              \
        _suite_failed = tests_failed; \
    } while (0)

#define TEST_SUITE_END(name)                                                                \
    do {                                                                                    \
        if (tests_failed == _suite_failed)                                                  \
            pr_info("test: %s [OK]\n", name);                                               \
        else                                                                                \
            pr_err("test: %s: %d failures [FAILED]\n", name, tests_failed - _suite_failed); \
    } while (0)

void run_all_tests(void);

// lib tests
void test_types(void);
void test_string(void);

// kernel tests
void test_spinlock(void);
void test_pmm(void);
void test_slab(void);
void test_heap(void);
void test_timer(void);
void test_sd(void);
void test_mmu(void);
void test_mmu_user(void);
void test_vfs(void);
void test_fat32(void);
void test_pipe(void);
void test_mutex(void);
void test_uaccess(void);
void test_scheduler(void);

// scheduler tests (must be called after enable_interrupts + sched_init)
void run_scheduler_tests(void);

/* post-init tests (must be called after a user process exists) */
void run_post_init_tests(void);
void test_signals(void);

void test_kasan_heap(void);
void test_kasan_slab(void);

#endif // _TEST_H_
