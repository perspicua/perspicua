/*
 * semaphore.c - Implementation of a counting semaphore built on the generic wait queue.
 */

#include "core/semaphore.h"

#include <limits.h>
#include "panic.h"
#include "sched/wait.h"

void ksem_init(struct ksem *s, int count)
{
    if (count < 0) {
        PANIC("semaphore: negative count");
    }

    s->count = count;
    wq_init(&s->wq);
}

int ksem_trydown(struct ksem *s)
{
    int count = __atomic_load_n(&s->count, __ATOMIC_ACQUIRE);
    while (count > 0) {
        int expected = count;
        if (__atomic_compare_exchange_n(&s->count, &expected, count - 1, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            return 1;
        }
        count = expected;
    }
    return 0;
}

void ksem_down(struct ksem *s)
{
    if (ksem_trydown(s)) {
        return;
    }

    wq_wait_event(&s->wq, ksem_trydown(s));
}

int ksem_down_interruptible(struct ksem *s)
{
    if (ksem_trydown(s)) {
        return 0;
    }

    return wq_wait_event_interruptible(&s->wq, ksem_trydown(s));
}

int ksem_down_timeout(struct ksem *s, unsigned long timeout_ms)
{
    if (ksem_trydown(s)) {
        return 0;
    }

    if (timeout_ms == 0) {
        return -ETIMEDOUT;
    }

    return wq_wait_event_timeout(&s->wq, ksem_trydown(s), timeout_ms);
}

void ksem_up(struct ksem *s)
{
    int old_count = __atomic_fetch_add(&s->count, 1, __ATOMIC_SEQ_CST);

    if (old_count == INT_MAX) {
        PANIC("semaphore: count cannot exceed INT_MAX");
    }

    wq_wake_one(&s->wq);
}
