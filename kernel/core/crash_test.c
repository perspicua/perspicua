/*
 * crash_test.c - Boot-time deliberate crash triggers for test verification.
 */

#include "debug/crash_test.h"

#include "stdio.h"
#include "string.h"

#include "sched/sched.h"

static volatile int crash_test_limit = 1000000;

__attribute__((noinline)) static void recurse_stack(int depth)
{
    volatile char buf[256];
    buf[0] = (char)depth;
    if (depth >= crash_test_limit) {
        return;
    }
    recurse_stack(depth + 1);
    buf[255] = buf[0];
}

static void crash_test_stack_task(void)
{
    recurse_stack(1);
}

void crash_test_start(void)
{
    if (CONFIG_CRASH_TEST[0] == '\0') {
        return;
    }

    if (strcmp(CONFIG_CRASH_TEST, "stack") == 0) {
        printk("crash-test: starting 'stack'\n");
        if (sched_create_task(crash_test_stack_task) != 0) {
            printk("crash-test: cannot start 'stack'\n");
        }
    }
}
