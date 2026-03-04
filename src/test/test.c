#include "test.h"

int tests_passed = 0;
int tests_failed = 0;
int _suite_failed = 0;

// run all pre-interrupt tests

void run_all_tests(void)
{
    tests_passed = 0;
    tests_failed = 0;

    printf("\n");
    printf(" perspicua kernel test suite\n");
    printf("\n");

    test_types();
    test_string();
    test_spinlock();
    test_pmm();
    test_heap();
    test_timer();

    printf("\n");

    if (tests_failed == 0)
    {
        printf("[  OK  ] All %d tests passed.\n", tests_passed);
        printf("[  OK  ] Reached target: Kernel Self-Test Complete\n");
    }
    else
    {
        printf("[FAILED] %d of %d tests failed.\n", tests_failed, tests_passed + tests_failed);
        printf("[FAILED] Kernel self-test incomplete — review failures above.\n");
    }

    printf("\n");
}

void run_scheduler_tests(void)
{
    int pre_passed = tests_passed;
    int pre_failed = tests_failed;

    printf("\n");

    test_scheduler();

    printf("\n");

    int sched_passed = tests_passed - pre_passed;
    int sched_failed = tests_failed - pre_failed;

    if (sched_failed == 0)
    {
        printf("[  OK  ] Scheduler: all %d tests passed.\n", sched_passed);
        printf("[  OK  ] Reached target: Scheduler Test Complete\n");
    }
    else
    {
        printf("[FAILED] Scheduler: %d of %d tests failed.\n", sched_failed, sched_passed + sched_failed);
    }

    printf("\n");
}
