/*
 * completion.c - Implementations of completion functions
 */

#include "core/completion.h"

#include "core/lockdep.h"
#include "sched/wait.h"
#include "panic.h"

// Never use on a completion that has waiters.
void completion_init(struct completion *c)
{
    c->done = 0;
    wq_init(&c->wq);
}

void completion_reinit(struct completion *c)
{
    unsigned long flags = spin_lock_irqsave(&c->wq.lock);
    if (c->wq.head != NULL) {
        PANIC("completion_reinit: waiters still queued");
    }

    __atomic_store_n(&c->done, 0, __ATOMIC_SEQ_CST);
    spin_unlock_irqrestore(&c->wq.lock, flags);
}

int completion_done(struct completion *c)
{
    unsigned int done = __atomic_load_n(&c->done, __ATOMIC_ACQUIRE);
    return done != 0;
}

int completion_try_wait(struct completion *c)
{
    unsigned int done = __atomic_load_n(&c->done, __ATOMIC_ACQUIRE);

    while (done > 0) {
        if (done == COMPLETION_ALL) {
            return 1;
        }

        unsigned int expected = done;

        if (__atomic_compare_exchange_n(&c->done, &expected, done - 1, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            return 1;
        }
        done = expected;
    }

    return 0;
}

void completion_wait(struct completion *c)
{
    lockdep_might_sleep();
    if (completion_try_wait(c)) {
        return;
    }

    wq_wait_event(&c->wq, completion_try_wait(c));
}

int completion_wait_interruptible(struct completion *c)
{
    lockdep_might_sleep();
    if (completion_try_wait(c)) {
        return 0;
    }

    return wq_wait_event_interruptible(&c->wq, completion_try_wait(c));
}

int completion_wait_timeout(struct completion *c, unsigned long timeout_ms)
{
    lockdep_might_sleep();
    if (completion_try_wait(c)) {
        return 0;
    }

    if (timeout_ms == 0) {
        return -ETIMEDOUT;
    }

    return wq_wait_event_timeout(&c->wq, completion_try_wait(c), timeout_ms);
}

void complete(struct completion *c)
{
    unsigned int done = __atomic_load_n(&c->done, __ATOMIC_ACQUIRE);

    for (;;) {
        unsigned int expected = done;
        if (done == COMPLETION_ALL) {
            break;
        }

        if (done == COMPLETION_ALL - 1) {
            PANIC("completion: done would overflow");
        }

        if (__atomic_compare_exchange_n(&c->done, &expected, done + 1, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            break;
        }
        done = expected;
    }

    wq_wake_one(&c->wq);
}

void complete_all(struct completion *c)
{
    __atomic_store_n(&c->done, COMPLETION_ALL, __ATOMIC_SEQ_CST);
    wq_wake_all(&c->wq);
}
