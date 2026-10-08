/*
 * rwsem.h - Reader/writer sleeping lock.
 *
 * Writer-preferring: once a writer is waiting, new readers queue behind it,
 * even though only readers hold the lock.
 */

#ifndef PERSPICUA_CORE_RWSEM_H
#define PERSPICUA_CORE_RWSEM_H

#include <stddef.h>

#include "core/lock.h"
#include "sched/wait.h"

struct task;

struct rwsem {
    spinlock_t guard;
    int readers;
    struct task *writer;
    int writers_waiting;
    struct wait_queue read_wq;
    struct wait_queue write_wq;
};

#define RWSEM_INIT {SPINLOCK_INIT, 0, NULL, 0, WAIT_QUEUE_INIT, WAIT_QUEUE_INIT}

void rwsem_init(struct rwsem *s);
void rwsem_down_read(struct rwsem *s);
void rwsem_up_read(struct rwsem *s);
void rwsem_down_write(struct rwsem *s);
void rwsem_up_write(struct rwsem *s);

#endif // PERSPICUA_CORE_RWSEM_H
