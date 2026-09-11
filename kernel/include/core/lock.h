/*
 * lock.h - Public API for synchronization and atomic primitives.
 */

#ifndef PERSPICUA_CORE_LOCK_H
#define PERSPICUA_CORE_LOCK_H

#include "types.h"

#include "arch/cpu.h"

#define SPINLOCK_INIT  {0}
#define ATOMIC_INIT(i) {(i)}

/*
 * struct spinlock_t - Simple busy-wait lock for short critical sections.
 *
 * Uses AArch64 exclusive monitors to provide mutual exclusion.
 */
typedef struct {
    volatile unsigned int locked;
} spinlock_t;

/*
 * struct atomic_t - Thread-safe integer counter.
 */
typedef struct {
    volatile int counter;
} atomic_t;

void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);
unsigned long spin_lock_irqsave(spinlock_t *lock);
void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags);

/*
 * preempt_active - True while the calling core holds at least one spinlock.
 *
 * The timer interrupt must not preempt a lock holder: a core spinning for that
 * lock would then be waiting on a task that is no longer scheduled.
 */
int preempt_active(void);

void atomic_set(atomic_t *a, int value);
void atomic_inc(atomic_t *a);
int atomic_dec_and_test(atomic_t *a);

#endif // PERSPICUA_CORE_LOCK_H
