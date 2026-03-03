#ifndef _SCHED_H_
#define _SCHED_H_

#define TASK_STACK_SIZE 4096
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

enum task_state
{
    TASK_RUNNING,
    TASK_READY,
    TASK_BLOCKED,
    TASK_DEAD
};

struct task
{
    struct cpu_context context;
    enum task_state state;
    unsigned long wake_time;
    unsigned long id;
    unsigned char* stack;

    struct task* next;
};

void sched_init(void);
void sched_create_task(void (*entry)(void));
void sched_sleep_ms(unsigned long ms);
void schedule(void);

// defined in switch.S
extern void switch_context(struct cpu_context* prev, struct cpu_context* next);

#endif // _SCHED_H_
