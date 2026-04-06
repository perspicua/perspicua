/*
 * sched.h - Public API for the kernel task scheduler.
 *
 * This file defines the task structures, scheduler states, and functions
 * responsible for multi-core preemptive multitasking and context switching.
 */

#ifndef PERSPICUA_SCHED_SCHED_H
#define PERSPICUA_SCHED_SCHED_H

#include "types.h"
#include "mm/pmm.h"

/* Stack protection and sizing constants */
#define SCHED_STACK_CANARY       0xDEADC0DEDEADC0DEULL
#define SCHED_STACK_GUARD_PAGES  1
#define SCHED_STACK_USABLE_PAGES 63
#define SCHED_STACK_PAGES        64 // Must be exact power of two for buddy allocator
#define SCHED_TASK_STACK_SIZE    (SCHED_STACK_USABLE_PAGES * PAGE_SIZE)

/* Possible execution states for a task. */
enum sched_task_state {
    SCHED_TASK_RUNNING,
    SCHED_TASK_READY,
    SCHED_TASK_BLOCKED,
    SCHED_TASK_DEAD
};

/* Saved processor state for context switching (AArch64 callee-saved). */
struct cpu_context {
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
    unsigned long fp; /* x29 */
    unsigned long lr; /* x30 */
    unsigned long sp;
};

/* Primary structure representing an execution thread (thread control block). */
struct task {
    struct cpu_context context;
    unsigned long ttbr0;         /* TTBR0 value containing user page table and ASID */
    enum sched_task_state state; /* Current execution state */
    unsigned long wake_time;     /* System time when a sleeping task should wake */
    unsigned long id;            /* Unique numeric task identifier */
    uint32_t pid;                /* Associated process identifier (0 for kernel tasks) */
    unsigned char *stack;        /* Pointer to the allocated stack region */
    struct task *next;           /* Link for ready and sleep queues */
    int skip_signals;            /* Flag to indicate if signal handling should be deferred */
    volatile int on_core;        /* CPU core ID currently running this task, or -1 */
};

/* Returns the index of the current CPU core (0-3). */
static inline int get_core_id(void)
{
    unsigned long mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (int)(mpidr & 3);
}

/* Appends a task to the end of a specific CPU's ready queue. */
void enqueue_ready(int cpu, struct task *t);

/* Initializes the scheduler on the primary CPU core. */
void sched_init(void);

/* Initializes the scheduler on a secondary CPU core. */
void sched_secondary_init(void);

/* Spawns a new kernel-mode task starting at the entry function. */
void sched_create_task(void (*entry)(void));

/* Initializes a task structure for a user-mode process. */
struct task *sched_create_user_task(unsigned long forged_sp, unsigned long forged_lr,
                                    uintptr_t kstack_base, uint32_t pid);

/* Puts the current task to sleep for a minimum number of milliseconds. */
void sched_sleep_ms(unsigned long ms);

/* Core scheduling algorithm: selects next task and performs context switch. */
void schedule(void);

/* Transitions the current task to blocked state and yields. */
void sched_block(void);

/* Transitions a specific task from blocked to ready state. */
void sched_unblock(struct task *t);

/* Returns a pointer to the task currently running on the calling CPU core. */
struct task *sched_get_current(void);

/* Returns the PID of the process currently occupying the specified CPU core. */
int sched_get_core_pid(int cpu);

/* Low-level assembly function to swap processor state. */
extern void switch_context(struct cpu_context *prev, struct cpu_context *next);

#ifdef CONFIG_NR_CPUS
    #define SCHED_NUM_CORES CONFIG_NR_CPUS
#else
    #define SCHED_NUM_CORES 4
#endif

/*
 * Scheduler statistics tracked per CPU core.
 */
struct sched_stats {
    uint64_t context_switches;
    uint64_t idle_count;
};

extern struct sched_stats core_sched_stats[SCHED_NUM_CORES];

#endif /* PERSPICUA_SCHED_SCHED_H */
