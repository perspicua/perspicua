#ifndef PERSPICUA_CORE_LOCKDEP_H
#define PERSPICUA_CORE_LOCKDEP_H

#include "core/lock.h"

/*
 * lockdep_init - Initializes the lock dependency validator.
 */
void lockdep_init(void);

/*
 * lockdep_acquire - Called BEFORE a thread attempts to acquire a lock.
 * Records the dependency and panics if a cycle is detected.
 */
void lockdep_acquire(spinlock_t *lock);

/*
 * lockdep_release - Called AFTER a thread releases a lock.
 * Removes the lock from the thread's held locks tracking.
 */
void lockdep_release(spinlock_t *lock);

#endif /* PERSPICUA_CORE_LOCKDEP_H */
