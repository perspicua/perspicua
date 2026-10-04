/*
 * wait.h - Generic wait queue implementation.
 */

#ifndef PERSPICUA_SCHED_WAIT_H
#define PERSPICUA_SCHED_WAIT_H

#include <stddef.h>

#include "uapi/errno.h"

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

// Deadlines are absolute system times in ms; 0 means no timeout.
unsigned long wq_deadline(unsigned long timeout_ms);
int wq_timed_out(unsigned long deadline);
void wq_timeout_arm(unsigned long deadline);
void wq_timeout_cancel(void);

#define __wq_wait_event(wq, cond, interruptible, has_lock, lock, timeout_ms) \
    ({                                                                       \
        int __ret = 0;                                                       \
        unsigned long __deadline = wq_deadline(timeout_ms);                  \
        struct wait_entry __e = {0};                                         \
        for (;;) {                                                           \
            unsigned long __flags = 0;                                       \
            if (!(has_lock)) {                                               \
                __flags = irq_save();                                        \
            }                                                                \
            wq_prepare((wq), &__e);                                          \
            if (cond) {                                                      \
                __ret = 0;                                                   \
            } else if ((interruptible) && wq_signal_pending()) {             \
                __ret = -ERESTARTSYS;                                        \
            } else if (__deadline && wq_timed_out(__deadline)) {             \
                __ret = -ETIMEDOUT;                                          \
            } else {                                                         \
                if (__deadline) {                                            \
                    wq_timeout_arm(__deadline);                              \
                }                                                            \
                if (has_lock) {                                              \
                    spin_unlock(lock);                                       \
                }                                                            \
                sched_schedule();                                            \
                if (has_lock) {                                              \
                    spin_lock(lock);                                         \
                } else {                                                     \
                    irq_restore(__flags);                                    \
                }                                                            \
                if (__deadline) {                                            \
                    wq_timeout_cancel();                                     \
                }                                                            \
                continue;                                                    \
            }                                                                \
            wq_finish((wq), &__e);                                           \
            if (!(has_lock)) {                                               \
                irq_restore(__flags);                                        \
            }                                                                \
            if (__ret && __e.woken) {                                        \
                wq_wake_one(wq);                                             \
            }                                                                \
            break;                                                           \
        }                                                                    \
        __ret;                                                               \
    })

#define wq_wait_event(wq, cond) __wq_wait_event(wq, cond, 0, 0, (spinlock_t *)NULL, 0)

#define wq_wait_event_interruptible(wq, cond) __wq_wait_event(wq, cond, 1, 0, (spinlock_t *)NULL, 0)

// 0 once cond holds, or -ETIMEDOUT after timeout_ms.
#define wq_wait_event_timeout(wq, cond, timeout_ms) \
    __wq_wait_event(wq, cond, 0, 0, (spinlock_t *)NULL, timeout_ms)

// The caller must hold lock with IRQs masked (e.g. via spin_lock_irqsave).
#define wq_wait_event_locked(wq, cond, lock) __wq_wait_event(wq, cond, 0, 1, lock, 0)

// The caller must hold lock with IRQs masked (e.g. via spin_lock_irqsave).
#define wq_wait_event_interruptible_locked(wq, cond, lock) __wq_wait_event(wq, cond, 1, 1, lock, 0)

#endif // PERSPICUA_SCHED_WAIT_H
