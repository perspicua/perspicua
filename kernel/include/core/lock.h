/*
 * lock.h - Public API for synchronization and atomic primitives.
 */

#ifndef PERSPICUA_CORE_LOCK_H
#define PERSPICUA_CORE_LOCK_H

#include "arch/cpu.h"

#define SPINLOCK_INIT  {0}
#define ATOMIC_INIT(i) {(i)}

// Busy-wait lock for short critical sections; holding one disables preemption.
typedef struct {
    volatile unsigned int locked;
} spinlock_t;

typedef struct {
    volatile int counter;
} atomic_t;

void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);

// Takes the lock if it is free and returns 1, or returns 0 at once; release with spin_unlock.
int spin_trylock(spinlock_t *lock);
unsigned long spin_lock_irqsave(spinlock_t *lock);
void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags);

// True while the task holds a spinlock: switched out, it would leave its lock's waiters spinning.
int preempt_active(void);

void atomic_set(atomic_t *a, int value);
void atomic_inc(atomic_t *a);
int atomic_dec_and_test(atomic_t *a);

#endif // PERSPICUA_CORE_LOCK_H
