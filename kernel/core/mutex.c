/*
 * mutex.c - Recursive sleeping mutex built on the generic wait queue (sched/wait.h).
 *
 * A task blocks (sched_schedule()) rather than spins when the lock is contended, so a
 * kmutex may be held across blocking operations such as SD card I/O. The short
 * internal `guard` spinlock is never held across sched_schedule(), keeping lockdep's
 * per-core tracking clean.
 */

#include "core/mutex.h"

#include <stddef.h>

#include "core/lock.h"
#include "panic.h"
#include "sched/sched.h"
#include "sched/wait.h"

void kmutex_init(struct kmutex *m)
{
    m->guard = (spinlock_t)SPINLOCK_INIT;
    m->owner = NULL;
    m->depth = 0;
    wq_init(&m->wq);
}

static int kmutex_try_lock(struct kmutex *m, struct task *self)
{
    unsigned long flags = spin_lock_irqsave(&m->guard);
    if (m->depth == 0) {
        m->owner = self;
        m->depth = 1;
        spin_unlock_irqrestore(&m->guard, flags);
        return 1;
    }
    if (m->owner == self) {
        m->depth++;
        spin_unlock_irqrestore(&m->guard, flags);
        return 1;
    }
    spin_unlock_irqrestore(&m->guard, flags);
    return 0;
}

void kmutex_lock(struct kmutex *m)
{
    struct task *self = sched_current_task();

    if (kmutex_try_lock(m, self)) {
        return;
    }

    wq_wait_event(&m->wq, kmutex_try_lock(m, self));
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
    if (m->owner != sched_current_task()) {
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

    spin_unlock_irqrestore(&m->guard, flags);

    wq_wake_one(&m->wq);
}
