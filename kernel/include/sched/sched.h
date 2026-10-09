/*
 * sched.h - Public API for the kernel task scheduler.
 */

#ifndef PERSPICUA_SCHED_SCHED_H
#define PERSPICUA_SCHED_SCHED_H

#include <stdint.h>
#include "mm/pmm.h"

#include "arch/cpu.h"

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

/*
 * Kernel stacks. A "stack base" is always the address the allocator returned,
 * with the guard page at its front; the usable region starts one page in.
 * Every holder of a stack pointer stores that base, so none of them has to
 * know where the guard page ends.
 */
void *kstack_alloc(void);
void kstack_free(void *stack_base);

// Highest address a task's kernel stack can grow to, given its base.
static inline uintptr_t kstack_top(const void *stack_base)
{
    return (uintptr_t)stack_base + PAGE_SIZE + SCHED_TASK_STACK_SIZE;
}

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

#ifdef CONFIG_LOCKDEP
    #define LOCKDEP_TASK_HELD 8
#endif

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
    int skip_signals;
    volatile int on_core;

    int in_syscall;
    uint64_t syscall_arg0;

    // Deadline a rewound nanosleep resumes; 0 when there is none.
    unsigned long sleep_resume_at;

    // CPU time in ticks, banked at each switch-out; acct_seq is odd while a switch updates it.
    uint64_t run_ticks;
    uint64_t ran_since;
    unsigned int acct_seq;

    // Spinlocks held by this task; non-zero disables preemption.
    int preempt_count;
    int need_resched;

#ifdef CONFIG_LOCKDEP
    const void *lockdep_held[LOCKDEP_TASK_HELD];
    int lockdep_depth;
#endif
};

void sched_enqueue(int cpu, struct task *t);
void sched_init(void);
void sched_secondary_init(void);
void sched_create_task(void (*entry)(void));
struct task *sched_create_user_task(unsigned long forged_sp, unsigned long forged_lr,
                                    uintptr_t kstack_base, uint32_t pid);
void sched_sleep_ms(unsigned long ms);
unsigned long sched_sleep_ms_interruptible(unsigned long ms);
void sched_schedule(void);
void sched_unblock(struct task *t);
void sched_continue(struct task *t);
int sched_task_set_blocked(struct task *t);
int sched_task_set_stopped(struct task *t);
int sched_task_is_idle(const struct task *t);

// Wakes a BLOCKED task at deadline (ms) unless something wakes it first.
void sched_timeout_arm(struct task *t, unsigned long deadline);
void sched_timeout_cancel(struct task *t);
void sched_exit_current(void) __attribute__((noreturn));
struct task *sched_current_task(void);

// CPU time t has used, in timer ticks, including a slice it is running right now.
uint64_t sched_task_cpu_ticks(struct task *t);
// Busy and idle ticks of a core since it came online; -1 if it never did.
int sched_core_cpu_ticks(int cpu, uint64_t *busy, uint64_t *idle);
void sched_return_to_user(void);
int sched_get_core_pid(int cpu);

extern void switch_context(struct cpu_context *prev, struct cpu_context *next);

#ifdef CONFIG_TESTS
// True while a task is linked in the timed sleep queue.
int sched_test_in_sleep_queue(const struct task *t);

// True while a task is linked in any CPU run queue.
int sched_test_in_run_queue(const struct task *t);

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

extern struct sched_stats core_sched_stats[CPU_MAX_CORES];

#endif // PERSPICUA_SCHED_SCHED_H
