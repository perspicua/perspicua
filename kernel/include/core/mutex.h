/*
 * mutex.h - Recursive sleeping lock, safe to hold across blocking I/O.
 */

#ifndef PERSPICUA_CORE_MUTEX_H
#define PERSPICUA_CORE_MUTEX_H

#include <stddef.h>

#include "core/lock.h"
#include "sched/wait.h"

struct task;

struct kmutex {
    spinlock_t guard;
    struct task *owner;
    unsigned int depth;
    struct wait_queue wq;
};

#define KMUTEX_INIT {SPINLOCK_INIT, NULL, 0, WAIT_QUEUE_INIT}

void kmutex_init(struct kmutex *m);
void kmutex_lock(struct kmutex *m);
void kmutex_unlock(struct kmutex *m);

#endif // PERSPICUA_CORE_MUTEX_H
