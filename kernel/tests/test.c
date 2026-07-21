#include "test.h"

int tests_passed = 0;
int tests_failed = 0;
int _suite_failed = 0;

// run all pre-interrupt tests

void run_all_tests(void)
{
    tests_passed = 0;
    tests_failed = 0;

    printk("\n");
    pr_info("test: perspicua kernel test suite\n");
    printk("\n");

    test_types();
    test_string();
    test_spinlock();
    test_pmm();
    test_slab();
    test_heap();
    test_timer();
    test_sd();
    test_mmu();
    test_mmu_user();
    test_vfs();
    test_fat32();
    test_pipe();
    test_mutex();
    test_uaccess();
    // test_kasan_heap();
    // test_kasan_slab();

    printk("\n");

    if (tests_failed == 0) {
        pr_info("test: all %d tests passed [OK]\n", tests_passed);
        pr_info("test: reached target: kernel self-test complete [OK]\n");
    } else {
        pr_err("test: %d of %d tests failed [FAILED]\n", tests_failed, tests_passed + tests_failed);
        pr_err("test: kernel self-test incomplete — review failures above [FAILED]\n");
    }

    printk("\n");
}

void run_scheduler_tests(void)
{
    int pre_passed = tests_passed;
    int pre_failed = tests_failed;

    printk("\n");

    test_scheduler();

    printk("\n");

    int sched_passed = tests_passed - pre_passed;
    int sched_failed = tests_failed - pre_failed;

    if (sched_failed == 0) {
        pr_info("test: scheduler: all %d tests passed [OK]\n", sched_passed);
        pr_info("test: reached target: scheduler test complete [OK]\n");
    } else {
        pr_err("test: scheduler: %d of %d tests failed [FAILED]\n", sched_failed,
               sched_passed + sched_failed);
    }

    printk("\n");
}

/*
 * run_post_init_tests - Suites that need a live user process to exist.
 *
 * Called after init has been created, so these can target a real PID rather
 * than the boot task's placeholder pid 0.
 */
void run_post_init_tests(void)
{
    int pre_passed = tests_passed;
    int pre_failed = tests_failed;

    printk("\n");

    test_signals();

    printk("\n");

    int post_passed = tests_passed - pre_passed;
    int post_failed = tests_failed - pre_failed;

    if (post_failed == 0) {
        pr_info("test: post-init: all %d tests passed [OK]\n", post_passed);
        pr_info("test: reached target: post-init test complete [OK]\n");
    } else {
        pr_err("test: post-init: %d of %d tests failed [FAILED]\n", post_failed,
               post_passed + post_failed);
    }

    printk("\n");
}
