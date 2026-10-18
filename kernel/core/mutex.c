/*
 * mutex.c - Recursive sleeping mutex on a wait queue; its guard spinlock is never held asleep.
 */

#include "core/mutex.h"

#include <stddef.h>

#include "core/lock.h"
#include "core/lockdep.h"
#include "panic.h"
#include "sched/sched.h"
#include "sched/wait.h"

void kmutex_init(struct kmutex *m)
{
    m->guard = (spinlock_t)SPINLOCK_INIT;
    __atomic_store_n(&m->owner, NULL, __ATOMIC_RELAXED);
    m->depth = 0;
    wq_init(&m->wq);
}

static int kmutex_try_lock(struct kmutex *m, struct task *self)
{
    unsigned long flags = spin_lock_irqsave(&m->guard);
    if (m->depth == 0) {
        __atomic_store_n(&m->owner, self, __ATOMIC_RELEASE);
        m->depth = 1;
        spin_unlock_irqrestore(&m->guard, flags);
        return 1;
    }
    spin_unlock_irqrestore(&m->guard, flags);
    return 0;
}

void kmutex_lock(struct kmutex *m)
{
    struct task *self = sched_current_task();

    if (self && __atomic_load_n(&m->owner, __ATOMIC_ACQUIRE) == self) {
        lockdep_might_sleep();
        unsigned long flags = spin_lock_irqsave(&m->guard);
        m->depth++;
        spin_unlock_irqrestore(&m->guard, flags);
        return;
    }

    lockdep_acquire_sleep(m);

    if (kmutex_try_lock(m, self)) {
        return;
    }

    wq_wait_event(&m->wq, kmutex_try_lock(m, self));
}

void kmutex_unlock(struct kmutex *m)
{
    unsigned long flags = spin_lock_irqsave(&m->guard);

    // Releasing someone else's mutex would leave its owner running unprotected.
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
    __atomic_store_n(&m->owner, NULL, __ATOMIC_RELEASE);

    spin_unlock_irqrestore(&m->guard, flags);

    lockdep_release_sleep(m);

    wq_wake_one(&m->wq);
}
