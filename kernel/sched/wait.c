/*
 * wait.c - Generic wait queue implementation.
 */

#include "sched/wait.h"

#include <stddef.h>

#include "core/lock.h"
#include "core/signals.h"
#include "core/timer.h"
#include "panic.h"
#include "sched/process.h"
#include "sched/sched.h"

void wq_init(struct wait_queue *wq)
{
    wq->lock = (spinlock_t)SPINLOCK_INIT;
    wq->head = NULL;
    wq->tail = NULL;
}

void wq_prepare(struct wait_queue *wq, struct wait_entry *e)
{
    struct task *curr = sched_current_task();
    if (!curr) {
        PANIC("wq_prepare: scheduler not started");
    }
    if (sched_task_is_idle(curr)) {
        PANIC("wq_prepare: the idle task cannot wait");
    }

    unsigned long flags = spin_lock_irqsave(&wq->lock);

    if (e->queue == NULL) {
        e->queue = wq;
        e->next = NULL;
        e->woken = 0;
        e->task = curr;
        if (wq->tail) {
            wq->tail->next = e;
            wq->tail = e;
        } else {
            wq->head = e;
            wq->tail = e;
        }
    } else if (e->queue != wq) {
        PANIC("wq_prepare: entry already on another queue");
    }

    sched_task_set_blocked(curr);

    spin_unlock_irqrestore(&wq->lock, flags);

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void wq_finish(struct wait_queue *wq, struct wait_entry *e)
{
    unsigned long flags = spin_lock_irqsave(&wq->lock);

    if (e->queue == wq) {
        struct wait_entry *prev = NULL;
        struct wait_entry *scan = wq->head;
        while (scan && scan != e) {
            prev = scan;
            scan = scan->next;
        }
        if (!scan) {
            PANIC("wq_finish: entry not found in queue");
        }
        if (prev) {
            prev->next = e->next;
        } else {
            wq->head = e->next;
        }
        if (wq->tail == e) {
            wq->tail = prev;
        }
        e->queue = NULL;
        e->next = NULL;
    } else if (e->queue != NULL) {
        PANIC("wq_finish: entry linked to unexpected queue");
    }

    struct task *t = sched_current_task();
    if (t) {
        enum sched_task_state expected = SCHED_TASK_BLOCKED;
        __atomic_compare_exchange_n(&t->state, &expected, SCHED_TASK_RUNNING, 0, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST);
    }

    spin_unlock_irqrestore(&wq->lock, flags);
}

int wq_wake_one(struct wait_queue *wq)
{
    unsigned long flags = spin_lock_irqsave(&wq->lock);

    if (!wq->head) {
        spin_unlock_irqrestore(&wq->lock, flags);
        return 0;
    }

    struct wait_entry *e = wq->head;
    wq->head = e->next;
    if (wq->tail == e) {
        wq->tail = NULL;
    }

    e->next = NULL;
    e->queue = NULL;
    e->woken = 1;

    sched_unblock(e->task);

    spin_unlock_irqrestore(&wq->lock, flags);
    return 1;
}

void wq_wake_all(struct wait_queue *wq)
{
    unsigned long flags = spin_lock_irqsave(&wq->lock);

    struct wait_entry *curr = wq->head;
    wq->head = NULL;
    wq->tail = NULL;

    while (curr) {
        struct wait_entry *next = curr->next;
        curr->next = NULL;
        curr->queue = NULL;
        curr->woken = 1;
        sched_unblock(curr->task);
        curr = next;
    }

    spin_unlock_irqrestore(&wq->lock, flags);
}

int wq_signal_pending(void)
{
    struct process *p = process_current();
    return p ? signal_pending(p) : 0;
}

unsigned long wq_deadline(unsigned long timeout_ms)
{
    return timeout_ms ? timer_get_system_time() + timeout_ms : 0;
}

int wq_timed_out(unsigned long deadline)
{
    return (long)(timer_get_system_time() - deadline) >= 0;
}

void wq_timeout_arm(unsigned long deadline)
{
    sched_timeout_arm(sched_current_task(), deadline);
}

void wq_timeout_cancel(void)
{
    sched_timeout_cancel(sched_current_task());
}
