#ifndef _TEST_H_
#define _TEST_H_

#include "stdio.h"

extern int tests_passed;
extern int tests_failed;
extern int _suite_failed;

// test assertion macros

#define TEST_ASSERT(name, cond)                       \
    do                                                \
    {                                                 \
        if (!(cond))                                  \
        {                                             \
            printf("[FAILED] %s: %s\n", name, #cond); \
            tests_failed++;                           \
            return;                                   \
        }                                             \
    } while (0)

#define TEST_ASSERT_EQ(name, actual, expected)                            \
    do                                                                    \
    {                                                                     \
        long _a = (long)(actual);                                         \
        long _e = (long)(expected);                                       \
        if (_a != _e)                                                     \
        {                                                                 \
            printf("[FAILED] %s: expected %ld, got %ld\n", name, _e, _a); \
            tests_failed++;                                               \
            return;                                                       \
        }                                                                 \
    } while (0)

#define TEST_ASSERT_NEQ(name, actual, unexpected)                    \
    do                                                               \
    {                                                                \
        long _a = (long)(actual);                                    \
        long _u = (long)(unexpected);                                \
        if (_a == _u)                                                \
        {                                                            \
            printf("[FAILED] %s: unexpected value %ld\n", name, _u); \
            tests_failed++;                                          \
            return;                                                  \
        }                                                            \
    } while (0)

#define TEST_PASS(name) \
    do                  \
    {                   \
        tests_passed++; \
    } while (0)

#define TEST_SUITE_BEGIN(name)        \
    do                                \
    {                                 \
        _suite_failed = tests_failed; \
    } while (0)

#define TEST_SUITE_END(name)                                                          \
    do                                                                                \
    {                                                                                 \
        if (tests_failed == _suite_failed)                                            \
            printf("[  OK  ] %s\n", name);                                            \
        else                                                                          \
            printf("[FAILED] %s: %d failures\n", name, tests_failed - _suite_failed); \
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
void test_scheduler(void);

// scheduler tests (must be called after enable_interrupts + sched_init)
void run_scheduler_tests(void);

#endif  // _TEST_H_
