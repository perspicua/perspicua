#ifndef _SCHED_H_
#define _SCHED_H_

#include "lib/types.h"

#define TASK_STACK_SIZE 8192
#define MAX_TASKS 16

// callee-saved registers + sp + pc
struct cpu_context
{
    unsigned long x19;
    unsigned long x20;
    unsigned long x21;
    unsigned long x22;
    unsigned long x23;
    unsigned long x24;
    unsigned long x25;
    unsigned long x26;
    unsigned long x27;
    unsigned long x28;
    unsigned long fp; // x29
    unsigned long lr; // x30
    unsigned long sp;
};

typedef enum
{
    TASK_RUNNING,
    TASK_READY,
    TASK_BLOCKED,
    TASK_DEAD
} task_state_t;

struct task
{
    struct cpu_context context;
    unsigned long ttbr0; // TTBR0 value (phys PGD | ASID<<48), 0 = kernel-only
    task_state_t state;
    unsigned long wake_time;
    unsigned long id;
    uint32_t pid;
    unsigned char* stack;

    struct task* next;
};

void sched_init(void);
void sched_create_task(void (*entry)(void));
void sched_sleep_ms(unsigned long ms);
void schedule(void);
void sched_secondary_init(void);

void sched_block(void);
void sched_unblock(struct task* t);

void post_switch_hook(void);

struct task* sched_get_current(void);

void sched_create_user_task(unsigned long forged_sp, unsigned long forged_lr, uint32_t pid);
// defined in switch.S
extern void switch_context(struct cpu_context* prev, struct cpu_context* next);

#endif // _SCHED_H_
