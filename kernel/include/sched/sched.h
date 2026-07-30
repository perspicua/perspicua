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

#ifdef CONFIG_NR_CPUS
    #define SCHED_NUM_CORES CONFIG_NR_CPUS
#else
    #define SCHED_NUM_CORES 4
#endif

/*
 * Kernel stack per task, including one unmapped guard page at the bottom.
 * Measured peak use is ~11 KB -- an interrupt landing inside a filesystem
 * syscall -- so 60 KB usable leaves better than 5x headroom. The total must
 * stay a power of two for the buddy allocator.
 */
#define SCHED_STACK_CANARY       0xDEADC0DEDEADC0DEULL
#define SCHED_STACK_GUARD_PAGES  1
#define SCHED_STACK_USABLE_PAGES 15
#define SCHED_STACK_PAGES        16
#define SCHED_TASK_STACK_SIZE    (SCHED_STACK_USABLE_PAGES * PAGE_SIZE)

/* Possible execution states for a task. */
enum sched_task_state {
    SCHED_TASK_RUNNING,
    SCHED_TASK_READY,
    SCHED_TASK_BLOCKED,
    SCHED_TASK_STOPPED,
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
    struct task *rq_next;        /* Link for the per-core ready queue */
    struct task *sleep_next;     /* Link for the timed sleep queue */
    struct task *wait_next;      /* Link for a driver wait queue (tty/pipe/sd) */
    int skip_signals;            /* Flag to indicate if signal handling should be deferred */
    volatile int on_core;        /* CPU core ID currently running this task, or -1 */
};

/*
 * Returns the index of the current CPU core.
 *
 * Every per-core array is sized by SCHED_NUM_CORES, so the result is folded
 * into that range. Masking with a literal 3 indexed out of bounds whenever the
 * build was configured for fewer than four cores. smp_init only releases cores
 * below the bound, so in practice the fold never triggers -- it keeps a core
 * that should not be running from writing past the end of an array.
 */
static inline int get_core_id(void)
{
    unsigned long mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (int)(mpidr & 0xFF) % SCHED_NUM_CORES;
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

/* Transitions the current task to stopped state */
void sched_stop(void);

/* Transitions a specific task from stopped to ready state. */
void sched_continue(struct task *t);

/* Returns a pointer to the task currently running on the calling CPU core. */
struct task *sched_get_current(void);

/* Returns the PID of the process currently occupying the specified CPU core. */
int sched_get_core_pid(int cpu);

/* Low-level assembly function to swap processor state. */
extern void switch_context(struct cpu_context *prev, struct cpu_context *next);

#ifdef CONFIG_TESTS
/* True while a task is linked in the timed sleep queue. */
int sched_test_in_sleep_queue(const struct task *t);

/* TTBR0 the scheduler would install for a process. */
unsigned long sched_test_task_ttbr0_for(uint32_t pid);
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
