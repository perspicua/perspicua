/*
 * lock.c - Implementation of synchronization and atomic primitives.
 */

#include "core/lock.h"
#include "core/lockdep.h"

#include "panic.h"

#include "arch/exception.h"

#include "arch/irq.h"
#include "sched/sched.h"

// The count lives in the task, so it follows the task if it moves to another core.
void preempt_disable(void)
{
    struct task *t = sched_current_task();

    if (t == NULL) {
        return;
    }

    t->preempt_count++;

    asm volatile("" ::: "memory");
}

void preempt_enable(void)
{
    asm volatile("" ::: "memory");

    struct task *t = sched_current_task();

    if (t == NULL) {
        return;
    }

    t->preempt_count--;

    if (t->preempt_count < 0) {
        PANIC("preempt_enable: unbalanced unlock");
    }

    asm volatile("" ::: "memory");

    if (t->preempt_count == 0 && t->need_resched && irqs_enabled() && !irq_in_handler()) {
        sched_schedule();
    }
}

int preempt_active(void)
{
    struct task *t = sched_current_task();
    return t && t->preempt_count != 0;
}

void spin_lock(spinlock_t *lock)
{
    /* Before lockdep, not after: preemption inside lockdep's lock leaves other cores spinning on
     * it. */
    preempt_disable();
    lockdep_acquire(lock);

    unsigned int tmp;
    unsigned int one = 1;

    asm volatile("   sevl\n"                   // Pre-set event to fall through first wfe
                 "1: wfe\n"                    // Wait for event from spin_unlock
                 "2: ldaxr   %w0, [%1]\n"      // Load-acquire (read lock state)
                 "   cbnz    %w0, 1b\n"        // Busy? Back to wfe
                 "   stxr    %w0, %w2, [%1]\n" // Try to store 1 (locked)
                 "   cbnz    %w0, 2b\n"        // Failed? Retry monitor sequence
                 : "=&r"(tmp)
                 : "r"(&lock->locked), "r"(one)
                 : "memory");
}

static inline void spin_unlock_raw(spinlock_t *lock)
{
    lockdep_release(lock);

    asm volatile("   stlr    %w0, [%1]\n" // Store-release (0 -> unlocked)
                 "   sev\n"               // Signal event to wake waiters
                 :
                 : "r"(0), "r"(&lock->locked)
                 : "memory");
}

void spin_unlock(spinlock_t *lock)
{
    spin_unlock_raw(lock);
    preempt_enable();
}

unsigned long spin_lock_irqsave(spinlock_t *lock)
{
    unsigned long flags = irq_save();
    spin_lock(lock);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags)
{
    spin_unlock_raw(lock);
    irq_restore(flags);
    preempt_enable();
}

/*
 * atomic_set - Stores an initial value into an atomic.
 */
void atomic_set(atomic_t *a, int value)
{
    __atomic_store_n(&a->counter, value, __ATOMIC_RELAXED);
}

/*
 * atomic_inc - Atomically adds 1 to an atomic variable.
 */
void atomic_inc(atomic_t *a)
{
    __atomic_fetch_add(&a->counter, 1, __ATOMIC_RELAXED);
}

/*
 * atomic_dec_and_test - Atomically subtracts 1 and returns 1 if result is zero.
 *
 * Enforces acquire-release ordering so prior stores are visible before destruction.
 */
int atomic_dec_and_test(atomic_t *a)
{
    int now = __atomic_sub_fetch(&a->counter, 1, __ATOMIC_ACQ_REL);
    if (now < 0) {
        PANIC("atomic: refcount underflow");
    }
    return now == 0;
}
