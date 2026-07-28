/*
 * mutex.c - Recursive sleeping mutex built on the scheduler wait queue.
 *
 * A task blocks (schedule) rather than spins when the lock is contended, so a
 * kmutex may be held across blocking operations such as SD card I/O. The short
 * internal `guard` spinlock is never held across schedule(), keeping lockdep's
 * per-core tracking clean.
 */

#include "core/mutex.h"

#include "panic.h"

#include "core/lock.h"
#include "sched/sched.h"

/* Unlink a task from the waiter list if present. Caller holds m->guard. */
static void kmutex_wq_remove(struct kmutex *m, struct task *t)
{
    struct task **pp = &m->wait_head;
    struct task *prev = NULL;

    while (*pp) {
        if (*pp == t) {
            *pp = t->wait_next;
            if (m->wait_tail == t) {
                m->wait_tail = prev;
            }
            t->wait_next = NULL;
            return;
        }
        prev = *pp;
        pp = &(*pp)->wait_next;
    }
}

void kmutex_init(struct kmutex *m)
{
    m->guard = (spinlock_t)SPINLOCK_INIT;
    m->owner = NULL;
    m->depth = 0;
    m->wait_head = NULL;
    m->wait_tail = NULL;
}

void kmutex_lock(struct kmutex *m)
{
    struct task *self = sched_get_current();

    for (;;) {
        unsigned long flags = spin_lock_irqsave(&m->guard);

        /* Recursive re-acquisition by the current owner. */
        if (self && m->owner == self) {
            m->depth++;
            spin_unlock_irqrestore(&m->guard, flags);
            return;
        }

        /* A signal wake may have left us queued; drop the stale link. */
        if (self) {
            kmutex_wq_remove(m, self);
        }

        if (m->owner == NULL) {
            m->owner = self;
            m->depth = 1;
            spin_unlock_irqrestore(&m->guard, flags);
            return;
        }

        if (!self) {
            /* Pre-scheduler context: no task to switch to, so spin. */
            spin_unlock_irqrestore(&m->guard, flags);
            continue;
        }

        self->state = SCHED_TASK_BLOCKED;
        self->wait_next = NULL;
        if (m->wait_tail) {
            m->wait_tail->wait_next = self;
            m->wait_tail = self;
        } else {
            m->wait_head = m->wait_tail = self;
        }

        spin_unlock_irqrestore(&m->guard, flags);
        schedule();
    }
}

void kmutex_unlock(struct kmutex *m)
{
    unsigned long flags = spin_lock_irqsave(&m->guard);

    /*
     * Releasing a mutex held by someone else hands the lock away and leaves the
     * real owner running unprotected inside its critical section. Nothing else
     * would notice until the corruption surfaced somewhere unrelated.
     */
    if (m->depth == 0) {
        spin_unlock_irqrestore(&m->guard, flags);
        PANIC("kmutex: unlock of a mutex that is not held");
    }
    if (m->owner != sched_get_current()) {
        spin_unlock_irqrestore(&m->guard, flags);
        PANIC("kmutex: unlock by a task that does not own the mutex");
    }

    if (m->depth > 1) {
        m->depth--;
        spin_unlock_irqrestore(&m->guard, flags);
        return;
    }

    m->depth = 0;
    m->owner = NULL;

    struct task *t = m->wait_head;
    if (t) {
        m->wait_head = t->wait_next;
        if (!m->wait_head) {
            m->wait_tail = NULL;
        }
        t->wait_next = NULL;
        sched_unblock(t);
    }

    spin_unlock_irqrestore(&m->guard, flags);
}
