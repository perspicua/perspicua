/*
 * lockdep.h - Lock-order validator; inline no-op stubs when CONFIG_LOCKDEP is off.
 */

#ifndef PERSPICUA_CORE_LOCKDEP_H
#define PERSPICUA_CORE_LOCKDEP_H

#include "core/lock.h"

#ifdef CONFIG_LOCKDEP

void lockdep_init(void);

// Before taking lock: records the dependency and panics on a cycle.
void lockdep_acquire(spinlock_t *lock);

// After releasing lock.
void lockdep_release(spinlock_t *lock);

void lockdep_might_sleep(void);
void lockdep_acquire_sleep(const void *lock);
void lockdep_release_sleep(const void *lock);
void lockdep_assert_no_sleep_locks(const char *where);
void lockdep_assert_preemptible(void);

    #ifdef CONFIG_TESTS
void lockdep_test_quiet(int on);
int lockdep_test_violations(void);
    #endif // CONFIG_TESTS

#else // !CONFIG_LOCKDEP

static inline void lockdep_init(void) {}
static inline void lockdep_acquire(spinlock_t *lock)
{
    (void)lock;
}
static inline void lockdep_release(spinlock_t *lock)
{
    (void)lock;
}

static inline void lockdep_might_sleep(void) {}
static inline void lockdep_acquire_sleep(const void *lock)
{
    (void)lock;
}
static inline void lockdep_release_sleep(const void *lock)
{
    (void)lock;
}
static inline void lockdep_assert_no_sleep_locks(const char *where)
{
    (void)where;
}
static inline void lockdep_assert_preemptible(void) {}

    #ifdef CONFIG_TESTS
static inline void lockdep_test_quiet(int on)
{
    (void)on;
}
static inline int lockdep_test_violations(void)
{
    return 0;
}
    #endif // CONFIG_TESTS

#endif // CONFIG_LOCKDEP

#endif // PERSPICUA_CORE_LOCKDEP_H
