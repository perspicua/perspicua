/*
 * failinject.h - One-shot allocation refusals for the test build.
 */

#ifndef PERSPICUA_MM_FAILINJECT_H
#define PERSPICUA_MM_FAILINJECT_H

#ifdef CONFIG_TESTS

    #include "sched/sched.h"

struct fail_arming {
    unsigned long countdown;
    const struct task *task;
};

// Returns what was left of the previous arming: 0 once it fired, or if there was none.
static inline unsigned long fail_arm(struct fail_arming *a, unsigned long n)
{
    a->task = sched_current_task();
    return __atomic_exchange_n(&a->countdown, n, __ATOMIC_ACQ_REL);
}

// Only the arming task's requests count, so no other core can spend or receive the refusal.
static inline int fail_fire(struct fail_arming *a)
{
    unsigned long n = __atomic_load_n(&a->countdown, __ATOMIC_ACQUIRE);

    while (n != 0) {
        if (sched_current_task() != a->task) {
            return 0;
        }
        if (__atomic_compare_exchange_n(&a->countdown, &n, n - 1, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            return n == 1;
        }
    }
    return 0;
}

#endif

#endif // PERSPICUA_MM_FAILINJECT_H
