#ifndef _LOCK_H_
#define _LOCK_H_

#include "lib/types.h"
typedef struct
{
    volatile unsigned int locked;
} spinlock_t;

#define SPINLOCK_INIT {0}

void spin_lock(spinlock_t* lock);
void spin_unlock(spinlock_t* lock);

unsigned long spin_lock_irqsave(spinlock_t* lock);
void spin_unlock_irqrestore(spinlock_t* lock, unsigned long flags);

void atomic_inc(atomic_t* a);
int atomic_dec_and_test(atomic_t* a);

#endif // _LOCK_H_
