/*
 * lock.c - Implementation of synchronization and atomic primitives.
 *
 * This module contains the low-level AArch64 implementations for
 * mutual exclusion and atomic arithmetic using exclusive monitors.
 */

#include "core/lock.h"
#include "core/lockdep.h"

#include "panic.h"

#include "arch/exception.h"

#include "core/timer.h"

/*
 * Per-core count of spinlocks held. The timer interrupt consults this before
 * preempting: a task holding a spinlock must run on until it releases, or a
 * core spinning for that lock waits on a task that is no longer scheduled.
 *
 * Raised before the acquire rather than after, so there is no window where the
 * lock is held but the count does not yet say so. Acquire and release always
 * happen on the same core, because preemption is exactly what this prevents.
 */
static volatile int preempt_count[SPINLOCK_MAX_CORES];

static inline int preempt_core(void)
{
    unsigned long mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (int)(mpidr & (SPINLOCK_MAX_CORES - 1));
}

/*
 * preempt_active - True while this core holds at least one spinlock.
 */
int preempt_active(void)
{
    return preempt_count[preempt_core()] != 0;
}

/*
 * spin_lock - Uses load-acquire/store-exclusive for AArch64 mutual exclusion.
 */
void spin_lock(spinlock_t *lock)
{
    lockdep_acquire(lock);
    preempt_count[preempt_core()]++;

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

    preempt_count[preempt_core()]--;
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
 *
 * Relaxed: taking a reference requires already holding one, so there is
 * nothing for this to order against.
 */
void atomic_inc(atomic_t *a)
{
    __atomic_fetch_add(&a->counter, 1, __ATOMIC_RELAXED);
}

/*
 * atomic_dec_and_test - Atomically subtracts 1 and returns 1 if result is zero.
 *
 * Acquire-release: the release half publishes everything this core did to the
 * object before the count dropped, and the acquire half makes those writes
 * visible to whichever core observes zero and destroys it. The hand-written
 * version used stxr with no release, so a reader could free an object while
 * another core's stores to it were still in flight.
 */
int atomic_dec_and_test(atomic_t *a)
{
    int now = __atomic_sub_fetch(&a->counter, 1, __ATOMIC_ACQ_REL);
    if (now < 0) {
        PANIC("atomic: refcount underflow");
    }
    return now == 0;
}
