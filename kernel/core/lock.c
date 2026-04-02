/*
 * lock.c - Implementation of synchronization and atomic primitives.
 *
 * This module contains the low-level AArch64 implementations for
 * mutual exclusion and atomic arithmetic using exclusive monitors.
 */

#include "core/lock.h"
#include "core/lockdep.h"

#include "arch/exception.h"

#include "core/timer.h"

/*
 * spin_lock - Uses load-acquire/store-exclusive for AArch64 mutual exclusion.
 */
void spin_lock(spinlock_t *lock)
{
    lockdep_acquire(lock);

    unsigned int tmp;
    unsigned int one = 1;

    asm volatile("   sevl\n"                   /* Pre-set event to fall through first wfe */
                 "1: wfe\n"                    /* Wait for event from spin_unlock */
                 "2: ldaxr   %w0, [%1]\n"      /* Load-acquire (read lock state) */
                 "   cbnz    %w0, 1b\n"        /* Busy? Back to wfe */
                 "   stxr    %w0, %w2, [%1]\n" /* Try to store 1 (locked) */
                 "   cbnz    %w0, 2b\n"        /* Failed? Retry monitor sequence */
                 : "=&r"(tmp)
                 : "r"(&lock->locked), "r"(one)
                 : "memory");
}

/*
 * spin_unlock - Uses store-release and SEV to signal other cores.
 */
void spin_unlock(spinlock_t *lock)
{
    lockdep_release(lock);

    asm volatile("   stlr    %w0, [%1]\n" /* Store-release (0 -> unlocked) */
                 "   sev\n"               /* Signal event to wake waiters */
                 :
                 : "r"(0), "r"(&lock->locked)
                 : "memory");
}

/*
 * spin_lock_irqsave - Saves local interrupt state and acquires lock.
 */
unsigned long spin_lock_irqsave(spinlock_t *lock)
{
    unsigned long flags = irq_save();
    spin_lock(lock);
    return flags;
}

/*
 * spin_unlock_irqrestore - Releases lock and restores local interrupt state.
 */
void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags)
{
    spin_unlock(lock);
    irq_restore(flags);
}

/*
 * atomic_inc - Atomically adds 1 to an atomic variable.
 */
void atomic_inc(atomic_t *a)
{
    int val, tmp;
    asm volatile("1: ldaxr   %w0, [%2]\n"
                 "   add     %w0, %w0, #1\n"
                 "   stxr    %w1, %w0, [%2]\n"
                 "   cbnz    %w1, 1b\n"
                 : "=&r"(val), "=&r"(tmp)
                 : "r"(&a->counter)
                 : "memory");
}

/*
 * atomic_dec_and_test - Atomically subtracts 1 and returns 1 if result is zero.
 */
int atomic_dec_and_test(atomic_t *a)
{
    int val, tmp, result;
    asm volatile("1: ldaxr   %w0, [%3]\n"
                 "   sub     %w0, %w0, #1\n"
                 "   stxr    %w1, %w0, [%3]\n"
                 "   cbnz    %w1, 1b\n"
                 "   cmp     %w0, #0\n"
                 "   cset    %w2, eq\n"
                 : "=&r"(val), "=&r"(tmp), "=r"(result)
                 : "r"(&a->counter)
                 : "memory");
    return result;
}
