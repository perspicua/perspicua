/*
 * mutex.h - Sleeping mutual-exclusion lock.
 *
 * Unlike a spinlock, a task may sleep (call schedule) while holding a kmutex,
 * so it is safe to hold across blocking I/O. Recursive: the owning task may
 * re-acquire it without deadlocking.
 */

#ifndef PERSPICUA_CORE_MUTEX_H
#define PERSPICUA_CORE_MUTEX_H

#include "types.h"

#include "core/lock.h"

struct task;

/*
 * struct kmutex - Recursive sleeping lock backed by a scheduler wait queue.
 */
struct kmutex {
    spinlock_t guard;       /* Protects the fields below */
    struct task *owner;     /* Task currently holding the lock, or NULL */
    unsigned int depth;     /* Recursion count held by the owner */
    struct task *wait_head; /* FIFO of blocked waiters (via task->wait_next) */
    struct task *wait_tail;
};

#define KMUTEX_INIT {SPINLOCK_INIT, NULL, 0, NULL, NULL}

void kmutex_init(struct kmutex *m);
void kmutex_lock(struct kmutex *m);
void kmutex_unlock(struct kmutex *m);

#endif /* PERSPICUA_CORE_MUTEX_H */
