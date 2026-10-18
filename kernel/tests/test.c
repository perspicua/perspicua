/*
 * test.c - The self-test runners: boot-task suites, multi-task suites, then post-init suites.
 */

#include "test.h"

int tests_passed = 0;
int tests_failed = 0;
int test_suite_failed = 0;
int test_suite_passed = 0;

void run_all_tests(void)
{
    tests_passed = 0;
    tests_failed = 0;

    printk("\n");
    pr_info("test: perspicua kernel test suite\n");
    printk("\n");

    test_types();
    test_string();
    test_ksym();
    test_spinlock();
    test_pmm();
    test_slab();
    test_heap();
    test_heap_props();
    test_timer();
    test_sd();
    test_mmu();
    test_mmu_user();
    test_vfs();
    test_fat32();
    test_fat32_corrupt();
    test_pipe();
    test_procfs();
    test_mutex();
    test_semaphore();
    test_completion();
    test_rwsem();
    test_wait();
    test_uaccess();
    test_process();
    test_elf();
    test_syscall_bounds();
    test_faultinject();

    printk("\n");

    if (tests_failed == 0) {
        pr_info("test: all %d tests passed [OK]\n", tests_passed);
    } else {
        pr_err("test: %d of %d tests failed [FAILED]\n", tests_failed, tests_passed + tests_failed);
        pr_err("test: kernel self-test incomplete — review failures above [FAILED]\n");
    }
    pr_info("test: reached target: kernel self-test complete\n");

    printk("\n");
}

void run_scheduler_tests(void)
{
    int pre_passed = tests_passed;
    int pre_failed = tests_failed;

    printk("\n");

    test_scheduler();
    test_wait_scheduler();
    test_semaphore_scheduler();
    test_completion_scheduler();
    test_rwsem_scheduler();
    test_spinlock_scheduler();
    test_tty_scheduler();
    test_lockdep_scheduler();
    test_lockdep_spin();

    printk("\n");

    int sched_passed = tests_passed - pre_passed;
    int sched_failed = tests_failed - pre_failed;

    if (sched_failed == 0) {
        pr_info("test: scheduler: all %d tests passed [OK]\n", sched_passed);
    } else {
        pr_err("test: scheduler: %d of %d tests failed [FAILED]\n", sched_failed,
               sched_passed + sched_failed);
    }
    pr_info("test: reached target: scheduler test complete\n");

    printk("\n");
}

// Suites that target a live user process, so they run once init exists.
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
    } else {
        pr_err("test: post-init: %d of %d tests failed [FAILED]\n", post_failed,
               post_passed + post_failed);
    }
    pr_info("test: reached target: post-init test complete\n");

    printk("\n");
}
