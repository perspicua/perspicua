/*
 * sched.c - Preemptive, SMP-aware round-robin scheduler for AArch64.
 *
 * This file implements task queue management, sleep handling, work-stealing,
 * and context switching for the kernel.
 */

#include "sched/sched.h"

#include "mm/asid.h"
#include "types.h"
#include "stdio.h"
#include "string.h"
#include "panic.h"

#include "uapi/errors.h"

#include "mm/mmu.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "mm/addr.h"
#include "core/timer.h"
#include "core/lock.h"
#include "sched/process.h"

_Static_assert(sizeof(struct cpu_context) == 104, "cpu_context size mismatch — update switch.S");
_Static_assert(__builtin_offsetof(struct task, state) == 112,
               "task->state offset mismatch — update task_wrapper_asm");
_Static_assert(SCHED_TASK_DEAD == 4, "task_wrapper_asm stores this value literally");

#define TASK_CANARY_PTR(t) ((unsigned long *)((t)->stack + PAGE_SIZE))

/* Sentinels to detect BSS corruption. */
static uint64_t s_canary_lo = 0xAAAAAAAAAAAAAAAAULL;
static struct task sched_boot_tasks[SCHED_NUM_CORES];
static uint64_t s_canary_hi = 0xBBBBBBBBBBBBBBBBULL;

/* Per-core idle tasks and ready queues. */
static struct task *sched_idle[SCHED_NUM_CORES];
static struct task *sched_rq_head[SCHED_NUM_CORES];
static struct task *sched_rq_tail[SCHED_NUM_CORES];
static spinlock_t sched_rq_lock[SCHED_NUM_CORES] = {[0 ... SCHED_NUM_CORES - 1] = SPINLOCK_INIT};

/* Global sleep queue (ordered by wake_time). */
static struct task *sched_sleep_head;
static spinlock_t sched_sleep_lock = SPINLOCK_INIT;

/* Per-core state tracking. */
static struct task *sched_cleanup[SCHED_NUM_CORES];
static struct task *sched_prev_task[SCHED_NUM_CORES];
static int sched_core_pid[SCHED_NUM_CORES];

struct sched_stats core_sched_stats[SCHED_NUM_CORES];

/* Task ID allocation. */
static unsigned long sched_next_id;
static spinlock_t sched_id_lock = SPINLOCK_INIT;

extern void task_wrapper_asm(void);

/* Verifies BSS sentinels around boot tasks. */
static void sched_check_bss_canaries(void)
{
    if (s_canary_lo != 0xAAAAAAAAAAAAAAAAULL || s_canary_hi != 0xBBBBBBBBBBBBBBBBULL) {
        pr_err("sched: BSS canary corruption!\n");
        PANIC("sched: memory corruption around boot task structs");
    }
}

/* Verifies that a task's kernel stack hasn't overflowed. */
static void task_check_stack_canary(const struct task *t)
{
    if (t->stack && *TASK_CANARY_PTR(t) != SCHED_STACK_CANARY) {
        pr_err("sched: stack overflow in task id=%lu pid=%u\n", t->id, (unsigned)t->pid);
        PANIC("sched: stack overflow detected via canary");
    }
}

/* Allocates a unique task identifier. */
static unsigned long alloc_task_id(void)
{
    unsigned long flags = spin_lock_irqsave(&sched_id_lock);
    unsigned long id = sched_next_id++;
    spin_unlock_irqrestore(&sched_id_lock, flags);
    return id;
}

/* Allocates a kernel stack with a guard page. */
static unsigned char *alloc_task_stack(void)
{
    unsigned char *base = (unsigned char *)pmm_alloc_pages(SCHED_STACK_PAGES);
    if (!base) {
        return NULL;
    }

    mmu_unmap_page((unsigned long)base); /* Guard page */
    *(unsigned long *)(base + PAGE_SIZE) = SCHED_STACK_CANARY;
    return base;
}

/* Frees a kernel stack and restores the guard page mapping. */
static void free_task_stack(unsigned char *stack)
{
    if (!stack) {
        return;
    }

    if ((unsigned long)stack < KERNEL_VMA) {
        PANIC("sched: corrupted t->stack in free_task_stack");
    }

    mmu_map_page((unsigned long)stack, V2P(stack), MMU_FLAGS_KERNEL_RW);
    pmm_free_pages(stack, SCHED_STACK_PAGES);
}

/* Sets up initial context for a new task. */
static void init_task_stack_context(struct task *t, unsigned char *stack, void (*entry)(void))
{
    unsigned long sp = ((unsigned long)(stack + PAGE_SIZE) + SCHED_TASK_STACK_SIZE) & ~15UL;
    t->stack = stack;
    t->context.sp = sp;
    t->context.lr = (unsigned long)task_wrapper_asm;
    t->context.x19 = (unsigned long)entry;
}

/* Internal enqueue helper. Caller must NOT hold rq lock. */
static void rq_enqueue(int cpu, struct task *t)
{
    unsigned long flags = spin_lock_irqsave(&sched_rq_lock[cpu]);

    t->state = SCHED_TASK_READY;
    t->rq_next = NULL;

    if (!sched_rq_head[cpu]) {
        sched_rq_head[cpu] = t;
        sched_rq_tail[cpu] = t;
    } else {
        sched_rq_tail[cpu]->rq_next = t;
        sched_rq_tail[cpu] = t;
    }

    spin_unlock_irqrestore(&sched_rq_lock[cpu], flags);
}

/* Internal dequeue helper. Returns first eligible task. */
static struct task *rq_dequeue(int cpu, int allow_pid0)
{
    unsigned long flags = spin_lock_irqsave(&sched_rq_lock[cpu]);
    struct task *prev = NULL;
    struct task *curr = sched_rq_head[cpu];

    while (curr) {
        if ((allow_pid0 || curr->pid != 0) && curr->on_core == -1) {
            if (prev) {
                prev->rq_next = curr->rq_next;
            } else {
                sched_rq_head[cpu] = curr->rq_next;
            }

            if (curr == sched_rq_tail[cpu]) {
                sched_rq_tail[cpu] = prev;
            }

            if (!sched_rq_head[cpu]) {
                sched_rq_tail[cpu] = NULL;
            }

            curr->rq_next = NULL;
            spin_unlock_irqrestore(&sched_rq_lock[cpu], flags);
            return curr;
        }
        prev = curr;
        curr = curr->rq_next;
    }

    spin_unlock_irqrestore(&sched_rq_lock[cpu], flags);
    return NULL;
}

/* Removes a task from the sleep queue if present. Caller holds sched_sleep_lock. */
static void sleep_unlink_locked(struct task *t)
{
    if (sched_sleep_head == t) {
        sched_sleep_head = t->sleep_next;
        t->sleep_next = NULL;
        return;
    }

    struct task *scan = sched_sleep_head;
    while (scan && scan->sleep_next != t) {
        scan = scan->sleep_next;
    }
    if (scan) {
        scan->sleep_next = t->sleep_next;
        t->sleep_next = NULL;
    }
}

/* Removes a task from the sleep queue, taking the lock itself. */
static void sleep_dequeue(struct task *t)
{
    unsigned long flags = spin_lock_irqsave(&sched_sleep_lock);
    sleep_unlink_locked(t);
    spin_unlock_irqrestore(&sched_sleep_lock, flags);
}

/* Inserts task into ordered sleep queue. */
static void sleep_enqueue(struct task *t)
{
    unsigned long flags = spin_lock_irqsave(&sched_sleep_lock);

    /*
     * A task woken early from sleep (e.g. by a signal delivering through
     * sched_unblock) stays linked in this queue until sleep_drain reaches its
     * wake_time. If it sleeps again before then, inserting it a second time
     * would splice the node into the list twice and form a cycle. Unlink any
     * stale entry first so a task appears at most once.
     */
    sleep_unlink_locked(t);

    if (!sched_sleep_head || (long)(t->wake_time - sched_sleep_head->wake_time) < 0) {
        t->sleep_next = sched_sleep_head;
        sched_sleep_head = t;
        spin_unlock_irqrestore(&sched_sleep_lock, flags);
        return;
    }

    struct task *curr = sched_sleep_head;
    while (curr->sleep_next && (long)(t->wake_time - curr->sleep_next->wake_time) >= 0) {
        curr = curr->sleep_next;
    }

    t->sleep_next = curr->sleep_next;
    curr->sleep_next = t;

    spin_unlock_irqrestore(&sched_sleep_lock, flags);
}

/* Returns expired tasks to ready queues. */
static void sleep_drain(int cpu)
{
    unsigned long now = get_system_time();
    unsigned long flags = spin_lock_irqsave(&sched_sleep_lock);

    while (sched_sleep_head && (long)(now - sched_sleep_head->wake_time) >= 0) {
        struct task *w = sched_sleep_head;
        sched_sleep_head = w->sleep_next;
        w->sleep_next = NULL;
        spin_unlock_irqrestore(&sched_sleep_lock, flags);

        /* Only enqueue if still blocked; prevents race with unblock. */
        enum sched_task_state expected = SCHED_TASK_BLOCKED;
        if (__atomic_compare_exchange_n(&w->state, &expected, SCHED_TASK_READY, 0, __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST)) {
            rq_enqueue(cpu, w);
        }

        flags = spin_lock_irqsave(&sched_sleep_lock);
    }

    spin_unlock_irqrestore(&sched_sleep_lock, flags);
}

/* Cleans up predecessor of a task that bypassed normal return path. */
static void idle_entry(void)
{
    int cpu = get_core_id();
    if (sched_prev_task[cpu]) {
        sched_prev_task[cpu]->on_core = -1;
        sched_prev_task[cpu] = NULL;
    }

    enable_interrupts();
    for (;;) {
        asm volatile("wfe");
    }
}

/* Allocates and initializes an idle task for a core. */
static struct task *create_idle_task(int core_id)
{
    struct task *t = (struct task *)heap_malloc(sizeof(struct task));
    if (!t) {
        PANIC("sched: OOM allocating idle task");
    }
    memset(t, 0, sizeof(*t));

    unsigned char *stack = alloc_task_stack();
    if (!stack) {
        PANIC("sched: OOM allocating idle task stack");
    }

    init_task_stack_context(t, stack, idle_entry);
    t->state = SCHED_TASK_READY;
    t->id = 900 + (unsigned long)core_id;
    t->pid = 0;
    t->ttbr0 = mmu_kernel_ttbr0();
    t->on_core = -1;

    return t;
}

/* Releases resources of a dead task. */
static void cleanup_dead_task(int cpu)
{
    struct task *dead = sched_cleanup[cpu];
    if (!dead) {
        return;
    }

    if (dead == sched_get_current()) {
        PANIC("sched: attempt to free the active task");
    }

    sched_cleanup[cpu] = NULL;

    if ((unsigned long)dead < KERNEL_VMA) {
        PANIC("sched: invalid task pointer in cleanup_dead_task");
    }

    if (dead->stack && (unsigned long)dead->stack < KERNEL_VMA) {
        PANIC("sched: t->stack corrupted before cleanup_dead_task");
    }

    /* Last line of defence: the sleep queue must not outlive the task. */
    sleep_dequeue(dead);

    free_task_stack(dead->stack);
    dead->stack = NULL;

    int is_boot = (dead >= &sched_boot_tasks[0] && dead < &sched_boot_tasks[SCHED_NUM_CORES]);
    if (!is_boot) {
        heap_free(dead);
    }
}

/* Appends a task to a ready queue. */
void enqueue_ready(int cpu, struct task *t)
{
    if (t && t->stack && (unsigned long)t->stack < KERNEL_VMA) {
        PANIC("sched: corrupted t->stack at enqueue");
    }
    rq_enqueue(cpu, t);
}

/* Initializes scheduler on the primary core. */
void sched_init(void)
{
    struct task *boot = &sched_boot_tasks[0];
    memset(boot, 0, sizeof(*boot));

    boot->state = SCHED_TASK_RUNNING;
    boot->id = alloc_task_id();
    boot->pid = 0;
    boot->stack = NULL; /* Uses boot stack */
    boot->ttbr0 = mmu_kernel_ttbr0();
    boot->on_core = 0;

    asm volatile("msr tpidr_el1, %0" ::"r"(boot));

    sched_idle[0] = create_idle_task(0);
    sched_core_pid[0] = 0;

    pr_info("sched: Initialized with %d cores\n", SCHED_NUM_CORES);
}

/* Initializes scheduler on secondary cores. */
void sched_secondary_init(void)
{
    int core_id = get_core_id();
    struct task *boot = &sched_boot_tasks[core_id];
    memset(boot, 0, sizeof(*boot));

    boot->state = SCHED_TASK_DEAD;
    boot->id = 800 + (unsigned long)core_id;
    boot->pid = 0;
    boot->ttbr0 = mmu_kernel_ttbr0();
    boot->on_core = core_id;

    asm volatile("msr tpidr_el1, %0" ::"r"(boot));

    sched_idle[core_id] = create_idle_task(core_id);
    sched_core_pid[core_id] = 0;

    enable_interrupts();
    schedule();

    PANIC("sched_secondary_init: schedule() returned unexpectedly");
}

/* Spawns a new kernel thread. */
void sched_create_task(void (*entry)(void))
{
    struct task *t = (struct task *)heap_malloc(sizeof(struct task));
    if (!t) {
        PANIC("sched_create_task: OOM for task struct");
    }
    memset(t, 0, sizeof(*t));

    unsigned char *stack = alloc_task_stack();
    if (!stack) {
        PANIC("sched_create_task: OOM for stack");
    }

    init_task_stack_context(t, stack, entry);
    t->state = SCHED_TASK_READY;
    t->ttbr0 = mmu_kernel_ttbr0();
    t->pid = 0;
    t->id = alloc_task_id();
    t->on_core = -1;

    rq_enqueue(get_core_id(), t);
}

/* Initializes a task for a user process. */
struct task *sched_create_user_task(unsigned long forged_sp, unsigned long forged_lr,
                                    uintptr_t kstack_base, uint32_t pid)
{
    struct task *t = (struct task *)heap_malloc(sizeof(struct task));
    if (!t) {
        return NULL;
    }
    memset(t, 0, sizeof(*t));

    t->state = SCHED_TASK_READY;
    t->context.sp = forged_sp;
    t->context.lr = forged_lr;
    t->pid = pid;
    t->id = alloc_task_id();
    t->on_core = -1;
    t->stack = (unsigned char *)(kstack_base - PAGE_SIZE);
    t->ttbr0 = process_get_ttbr0(pid);

    return t;
}

/* Puts the current task to sleep. */
void sched_sleep_ms(unsigned long ms)
{
    unsigned long flags = irq_save();
    int cpu = get_core_id();
    struct task *curr = sched_get_current();

    if (!curr || curr == sched_idle[cpu]) {
        irq_restore(flags);
        return;
    }

    curr->state = SCHED_TASK_BLOCKED;
    curr->wake_time = get_system_time() + ms;
    sleep_enqueue(curr);

    schedule();
    irq_restore(flags);
}

/* Yields the processor and enters BLOCKED state. */
void sched_block(void)
{
    unsigned long flags = irq_save();
    int cpu = get_core_id();
    struct task *curr = sched_get_current();

    if (!curr || curr == sched_idle[cpu]) {
        irq_restore(flags);
        return;
    }

    curr->state = SCHED_TASK_BLOCKED;
    schedule();
    irq_restore(flags);
}

/* Moves a blocked task to ready queue. */
void sched_unblock(struct task *t)
{
    if (!t) {
        return;
    }

    if (__atomic_load_n(&t->state, __ATOMIC_SEQ_CST) == SCHED_TASK_READY) {
        return;
    }

    enum sched_task_state expected = SCHED_TASK_BLOCKED;
    if (__atomic_compare_exchange_n(&t->state, &expected, SCHED_TASK_READY, 0, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST)) {
        /* A timed sleep leaves the task linked here. Drop it now: otherwise
         * sleep_drain can re-ready it out of an unrelated later block, or walk
         * it after the task has been freed. */
        sleep_dequeue(t);
        rq_enqueue(get_core_id(), t);
    }
}

void sched_stop(void)
{
    unsigned long flags = irq_save();
    int cpu = get_core_id();
    struct task *curr = sched_get_current();

    if (!curr || curr == sched_idle[cpu]) {
        irq_restore(flags);
        return;
    }

    curr->state = SCHED_TASK_STOPPED;
    schedule();
    irq_restore(flags);
}

void sched_continue(struct task *t)
{
    if (!t) {
        return;
    }

    if (__atomic_load_n(&t->state, __ATOMIC_SEQ_CST) == SCHED_TASK_READY) {
        return;
    }

    enum sched_task_state expected = SCHED_TASK_STOPPED;
    if (__atomic_compare_exchange_n(&t->state, &expected, SCHED_TASK_READY, 0, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST)) {
        sleep_dequeue(t);
        rq_enqueue(get_core_id(), t);
    }
}

/* Returns the active task on the calling CPU. */
struct task *sched_get_current(void)
{
    struct task *t;
    asm volatile("mrs %0, tpidr_el1" : "=r"(t));
    return t;
}

/* Returns the PID running on a specific core. */
int sched_get_core_pid(int cpu)
{
    if (cpu < 0 || cpu >= SCHED_NUM_CORES) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }
    return sched_core_pid[cpu];
}

#ifdef CONFIG_TESTS
int sched_test_in_sleep_queue(const struct task *t)
{
    unsigned long flags = spin_lock_irqsave(&sched_sleep_lock);
    int found = 0;

    for (struct task *scan = sched_sleep_head; scan; scan = scan->sleep_next) {
        if (scan == t) {
            found = 1;
            break;
        }
    }

    spin_unlock_irqrestore(&sched_sleep_lock, flags);
    return found;
}
#endif

/*
 * task_ttbr0_for - Builds the TTBR0 value for a process about to run.
 *
 * Read under the table lock: process_exit clears user_pgd while its task is
 * still runnable, and V2P(NULL) would otherwise be installed as a page table
 * base. A process that is no longer RUNNING has no address space worth
 * entering, so fall back to the kernel's.
 */
static unsigned long task_ttbr0_for(uint32_t pid)
{
    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    struct process *p = process_slot(pid);
    unsigned long ttbr0;

    if (p && p->state == PROCESS_STATE_RUNNING && p->user_pgd) {
        asid_get_active(&p->asid, &p->asid_generation);
        ttbr0 = V2P(p->user_pgd) | asid_ttbr_field(p->asid);
    } else {
        ttbr0 = mmu_kernel_ttbr0();
    }

    spin_unlock_irqrestore(&process_table_lock, flags);
    return ttbr0;
}

#ifdef CONFIG_TESTS
unsigned long sched_test_task_ttbr0_for(uint32_t pid)
{
    return task_ttbr0_for(pid);
}
#endif

/* Core scheduling logic. Selects next task and context switches. */
void schedule(void)
{
    unsigned long flags = irq_save();
    int cpu = get_core_id();

    /* Clean up predecessor if it bypassed normal return. */
    if (sched_prev_task[cpu]) {
        sched_prev_task[cpu]->on_core = -1;
        sched_prev_task[cpu] = NULL;
    }

    cleanup_dead_task(cpu);
    sleep_drain(cpu);
    sched_check_bss_canaries();

    struct task *prev = sched_get_current();
    if (!prev) {
        irq_restore(flags);
        return;
    }

    task_check_stack_canary(prev);

    switch (prev->state) {
        case SCHED_TASK_RUNNING:
            if (prev != sched_idle[cpu]) {
                rq_enqueue(cpu, prev);
            }
            break;
        case SCHED_TASK_BLOCKED:
        case SCHED_TASK_STOPPED:
        case SCHED_TASK_READY:
            break;
        case SCHED_TASK_DEAD:
            if (sched_cleanup[cpu]) {
                PANIC("sched: double-dead task on same core");
            }
            sched_cleanup[cpu] = prev;
            break;
    }

    struct task *next = rq_dequeue(cpu, 1);
    if (!next) {
        for (int i = 1; i <= SCHED_NUM_CORES; i++) {
            int victim = (cpu + i) % SCHED_NUM_CORES;
            next = rq_dequeue(victim, 0);
            if (next) {
                break;
            }
        }
    }

    if (!next) {
        next = sched_idle[cpu];
    }

    next->on_core = cpu;
    next->state = SCHED_TASK_RUNNING;
    sched_core_pid[cpu] = (int)next->pid;

    asm volatile("msr tpidr_el1, %0" ::"r"(next));

    if (next->pid > 0) {
        next->ttbr0 = task_ttbr0_for(next->pid);
    }

    asm volatile("msr ttbr0_el1, %0" ::"r"(next->ttbr0));
    asm volatile("dsb sy");
    asm volatile("isb");

    if (prev != next) {
        core_sched_stats[cpu].context_switches++;
        if (next == sched_idle[cpu]) {
            core_sched_stats[cpu].idle_count++;
        }

        if (next->context.lr == 0 || next->context.sp == 0) {
            PANIC("sched: corrupt task context");
        }

        sched_prev_task[cpu] = prev;
        switch_context(&prev->context, &next->context);

        /* Resumed on NEW stack. Re-fetch core ID in case of migration. */
        int new_cpu = get_core_id();
        if (sched_prev_task[new_cpu]) {
            sched_prev_task[new_cpu]->on_core = -1;
            sched_prev_task[new_cpu] = NULL;
        }
    }

    irq_restore(flags);
}
