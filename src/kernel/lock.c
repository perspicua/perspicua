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
                 "   sev\n"               // wake up threads waiting in wfe
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
