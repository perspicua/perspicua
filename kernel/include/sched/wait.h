/*
 * wait.h - Generic wait queue implementation.
 */

#ifndef PERSPICUA_SCHED_WAIT_H
#define PERSPICUA_SCHED_WAIT_H

#include <stddef.h>

#include "arch/irq.h"
#include "core/lock.h"
#include "core/signals.h"
#include "sched/sched.h"

#define WAIT_QUEUE_INIT {SPINLOCK_INIT, NULL, NULL}

struct wait_queue;

struct wait_entry {
    struct task *task;
    struct wait_entry *next;
    struct wait_queue *queue;
    int woken;
};

struct wait_queue {
    spinlock_t lock;
    struct wait_entry *head, *tail;
};

void wq_init(struct wait_queue *wq);
void wq_prepare(struct wait_queue *wq, struct wait_entry *e);
void wq_finish(struct wait_queue *wq, struct wait_entry *e);
int wq_wake_one(struct wait_queue *wq);
void wq_wake_all(struct wait_queue *wq);
int wq_signal_pending(void);

#define __wq_wait_event(wq, cond, interruptible, has_lock, lock) \
    ({                                                           \
        int __ret = 0;                                           \
        if (!sched_current_task()) {                             \
            while (!(cond)) {                                    \
                if (has_lock) {                                  \
                    spin_unlock(lock);                           \
                }                                                \
                __asm__ volatile("yield" ::: "memory");          \
                if (has_lock) {                                  \
                    spin_lock(lock);                             \
                }                                                \
            }                                                    \
        } else {                                                 \
            struct wait_entry __e = {0};                         \
            for (;;) {                                           \
                unsigned long __flags = 0;                       \
                if (!(has_lock)) {                               \
                    __flags = irq_save();                        \
                }                                                \
                wq_prepare((wq), &__e);                          \
                if (cond) {                                      \
                    wq_finish((wq), &__e);                       \
                    if (!(has_lock)) {                           \
                        irq_restore(__flags);                    \
                    }                                            \
                    __ret = 0;                                   \
                    break;                                       \
                }                                                \
                if ((interruptible) && wq_signal_pending()) {    \
                    wq_finish((wq), &__e);                       \
                    if (!(has_lock)) {                           \
                        irq_restore(__flags);                    \
                    }                                            \
                    if (__e.woken) {                             \
                        wq_wake_one(wq);                         \
                    }                                            \
                    __ret = -ERESTARTSYS;                        \
                    break;                                       \
                }                                                \
                if (has_lock) {                                  \
                    spin_unlock(lock);                           \
                }                                                \
                sched_schedule();                                \
                if (has_lock) {                                  \
                    spin_lock(lock);                             \
                } else {                                         \
                    irq_restore(__flags);                        \
                }                                                \
            }                                                    \
        }                                                        \
        __ret;                                                   \
    })

#define wq_wait_event(wq, cond) __wq_wait_event(wq, cond, 0, 0, (spinlock_t *)NULL)

#define wq_wait_event_interruptible(wq, cond) __wq_wait_event(wq, cond, 1, 0, (spinlock_t *)NULL)

// The caller must hold lock with IRQs masked (e.g. via spin_lock_irqsave).
#define wq_wait_event_locked(wq, cond, lock) __wq_wait_event(wq, cond, 0, 1, lock)

// The caller must hold lock with IRQs masked (e.g. via spin_lock_irqsave).
#define wq_wait_event_interruptible_locked(wq, cond, lock) __wq_wait_event(wq, cond, 1, 1, lock)

#endif // PERSPICUA_SCHED_WAIT_H
