/*
 * completion.h - One-shot or counted event that tasks wait for, built on the wait queue.
 */
#ifndef PERSPICUA_CORE_COMPLETION_H
#define PERSPICUA_CORE_COMPLETION_H

#include "sched/wait.h"
#include <limits.h>

#define COMPLETION_ALL UINT_MAX

struct completion {
    unsigned int done;
    struct wait_queue wq;
};

#define COMPLETION_INIT {0, WAIT_QUEUE_INIT}

void completion_init(struct completion *c);
void completion_reinit(struct completion *c);
int completion_done(struct completion *c);
int completion_try_wait(struct completion *c);
void completion_wait(struct completion *c);
int completion_wait_interruptible(struct completion *c);
int completion_wait_timeout(struct completion *c, unsigned long timeout_ms);
void complete(struct completion *c);
void complete_all(struct completion *c);

#endif // PERSPICUA_CORE_COMPLETION_H
