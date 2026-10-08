/*
 * rwsem.c - Writer-preferring reader/writer sleeping lock.
 */

#include "core/rwsem.h"

#include <stddef.h>

#include "core/lockdep.h"
#include "panic.h"
#include "sched/sched.h"

void rwsem_init(struct rwsem *s)
{
    s->guard = (spinlock_t)SPINLOCK_INIT;
    s->readers = 0;
    s->writer = NULL;
    s->writers_waiting = 0;
    wq_init(&s->read_wq);
    wq_init(&s->write_wq);
}

void rwsem_down_read(struct rwsem *s)
{
    lockdep_acquire_sleep(s);

    unsigned long flags = spin_lock_irqsave(&s->guard);
    if (s->writer || s->writers_waiting) {
        wq_wait_event_locked(&s->read_wq, !s->writer && s->writers_waiting == 0, &s->guard);
    }
    s->readers++;
    spin_unlock_irqrestore(&s->guard, flags);
}

void rwsem_up_read(struct rwsem *s)
{
    unsigned long flags = spin_lock_irqsave(&s->guard);
    if (s->readers == 0) {
        spin_unlock_irqrestore(&s->guard, flags);
        PANIC("rwsem: up_read without a reader");
    }

    s->readers--;
    int last_reader = (s->readers == 0);

    spin_unlock_irqrestore(&s->guard, flags);

    lockdep_release_sleep(s);

    if (last_reader) {
        wq_wake_one(&s->write_wq);
    }
}

void rwsem_down_write(struct rwsem *s)
{
    lockdep_acquire_sleep(s);

    struct task *self = sched_current_task();
    unsigned long flags = spin_lock_irqsave(&s->guard);
    s->writers_waiting++;

    if (s->writer || s->readers != 0) {
        wq_wait_event_locked(&s->write_wq, !s->writer && s->readers == 0, &s->guard);
    }

    s->writers_waiting--;
    s->writer = self;

    spin_unlock_irqrestore(&s->guard, flags);
}

void rwsem_up_write(struct rwsem *s)
{
    struct task *self = sched_current_task();
    unsigned long flags = spin_lock_irqsave(&s->guard);

    if (s->writer != self) {
        spin_unlock_irqrestore(&s->guard, flags);
        PANIC("rwsem: up_write by a task that is not the writer");
    }

    s->writer = NULL;
    int has_waiting_writers = (s->writers_waiting > 0);

    spin_unlock_irqrestore(&s->guard, flags);

    lockdep_release_sleep(s);

    if (has_waiting_writers) {
        wq_wake_one(&s->write_wq);
    } else {
        wq_wake_all(&s->read_wq);
    }
}
