#include "lock.h"
#include "timer.h"

void spin_lock(spinlock_t* lock)
{
    unsigned int tmp;
    unsigned int one = 1;

    asm volatile("   sevl\n"                   // set event flag so first wfe falls through
                 "1: wfe\n"                    // power-efficient wait
                 "2: ldaxr   %w0, [%1]\n"      // load-acquire exclusive
                 "   cbnz    %w0, 1b\n"        // if locked, go back to wfe
                 "   stxr    %w0, %w2, [%1]\n" // try to store 1
                 "   cbnz    %w0, 2b\n"        // if store failed, retry ldaxr
                 : "=&r"(tmp)
                 : "r"(&lock->locked), "r"(one)
                 : "memory");
}

void spin_unlock(spinlock_t* lock)
{
    asm volatile("   stlr    %w0, [%1]\n" // store-release (0 -> unlocked)
                 "   sev\n"               // signal event to wake waiting cores
                 :
                 : "r"(0), "r"(&lock->locked)
                 : "memory");
}

unsigned long spin_lock_irqsave(spinlock_t* lock)
{
    unsigned long flags = irq_save();
    spin_lock(lock);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t* lock, unsigned long flags)
{
    spin_unlock(lock);
    irq_restore(flags);
}

void atomic_inc(atomic_t* a)
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

int atomic_dec_and_test(atomic_t* a)
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
