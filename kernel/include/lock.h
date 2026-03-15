/*
 * lock.h - Public API for synchronization and atomic primitives.
 *
 * This file defines the spinlock and atomic types, as well as the
 * functions used to ensure thread safety across multiple CPU cores.
 */

#ifndef PERSPICUA_KERNEL_LOCK_H
#define PERSPICUA_KERNEL_LOCK_H

#include "types.h"

/*
 * spinlock_t - A simple busy-wait lock for short critical sections.
 */
typedef struct
{
    volatile unsigned int locked;
} spinlock_t;

/* Macro for static initialization of a spinlock. */
#define SPINLOCK_INIT {0}

/*
 * spin_lock - Acquires a spinlock. This function will busy-wait (spin)
 * until the lock becomes available.
 */
void spin_lock(spinlock_t* lock);

/*
 * spin_unlock - Releases a previously acquired spinlock and signals
 * other waiting cores.
 */
void spin_unlock(spinlock_t* lock);

/*
 * spin_lock_irqsave - Disables interrupts on the local core and
 * then acquires the spinlock. It returns the previous interrupt state.
 */
unsigned long spin_lock_irqsave(spinlock_t* lock);

/*
 * spin_unlock_irqrestore - Releases the spinlock and restores the
 * interrupt state to what it was before the lock was acquired.
 */
void spin_unlock_irqrestore(spinlock_t* lock, unsigned long flags);

/*
 * atomic_inc - Atomically increments the value of the provided atomic
 * variable using load-exclusive/store-exclusive instructions.
 */
void atomic_inc(atomic_t* a);

/*
 * atomic_dec_and_test - Atomically decrements the value of the atomic
 * variable and returns non-zero if the resulting value is zero.
 */
int atomic_dec_and_test(atomic_t* a);

#endif /* PERSPICUA_KERNEL_LOCK_H */
