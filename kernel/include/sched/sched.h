/*
 * sched.h - Public API for the kernel task scheduler.
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

// Possible execution states for a task.
enum sched_task_state {
    SCHED_TASK_RUNNING,
    SCHED_TASK_READY,
    SCHED_TASK_BLOCKED,
    SCHED_TASK_STOPPED,
    SCHED_TASK_DEAD
};

// Saved processor state for context switching (AArch64 callee-saved).
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
    unsigned long fp;
    unsigned long lr;
    unsigned long sp;
};

struct task {
    struct cpu_context context;
    unsigned long ttbr0;
    enum sched_task_state state;
    unsigned long wake_time;
    unsigned long id;
    uint32_t pid;
    unsigned char *stack;
    struct task *rq_next;
    struct task *sleep_next;
    struct task *wait_next;
    int skip_signals;
    volatile int on_core;
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

void enqueue_ready(int cpu, struct task *t);
void sched_init(void);
void sched_secondary_init(void);
void sched_create_task(void (*entry)(void));
struct task *sched_create_user_task(unsigned long forged_sp, unsigned long forged_lr,
                                    uintptr_t kstack_base, uint32_t pid);
void sched_sleep_ms(unsigned long ms);
void schedule(void);
void sched_block(void);
void sched_unblock(struct task *t);
void sched_stop(void);
void sched_continue(struct task *t);
struct task *sched_get_current(void);
int sched_get_core_pid(int cpu);

extern void switch_context(struct cpu_context *prev, struct cpu_context *next);

#ifdef CONFIG_TESTS
// True while a task is linked in the timed sleep queue.
int sched_test_in_sleep_queue(const struct task *t);

// TTBR0 the scheduler would install for a process.
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

#endif // PERSPICUA_SCHED_SCHED_H
