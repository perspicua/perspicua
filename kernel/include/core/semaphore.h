/*
 * semaphore.h - Counting semaphore built on the generic wait queue.
 */

#ifndef PERSPICUA_CORE_SEMAPHORE_H
#define PERSPICUA_CORE_SEMAPHORE_H

#include "sched/wait.h"

struct ksem {
    int count;
    struct wait_queue wq;
};

#define KSEM_INIT(n) {(n), WAIT_QUEUE_INIT}

void ksem_init(struct ksem *s, int count);
int ksem_trydown(struct ksem *s);
void ksem_down(struct ksem *s);
int ksem_down_interruptible(struct ksem *s);
int ksem_down_timeout(struct ksem *s, unsigned long timeout_ms);
void ksem_up(struct ksem *s);

#endif // PERSPICUA_CORE_SEMAPHORE_H
