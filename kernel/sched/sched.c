/*
 * sched.c - Implementation of the kernel task scheduler.
 *
 * This file contains the core logic for the preemptive scheduler, including
 * ready queue management, sleep queues, and the context switching engine.
 * It supports symmetric multiprocessing (SMP) with per-CPU ready queues.
 */

#include "sched/sched.h"

#include "uapi/errors.h"

#include "mm/mmu.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "mm/addr.h"
#include "core/timer.h"
#include "core/lock.h"
#include "sched/process.h"
#include "stdio.h"
#include "string.h"
#include "panic.h"

/* Static assertions to ensure structure layout matches low-level assembly assumptions */
_Static_assert(sizeof(struct cpu_context) == 104, "struct cpu_context size mismatch");
_Static_assert(__builtin_offsetof(struct task, state) == 112, "struct task->state offset mismatch");

/* Internal helper to locate the stack canary at the top of the guard page */
#define SCHED_TASK_CANARY_PTR(t) ((unsigned long*)((t)->stack + PAGE_SIZE))

/* Static scheduler state */
static struct task* sched_idle_tasks[4] = {NULL};

static uint64_t sched_canary_before = 0xAAAAAAAAAAAAAAAAULL;
static struct task sched_init_tasks[4];
static uint64_t sched_canary_after = 0xBBBBBBBBBBBBBBBBULL;

/* Per-CPU ready queues and locks */
static struct task* sched_ready_heads[4] = {NULL};
static struct task* sched_ready_tails[4] = {NULL};
static spinlock_t sched_ready_locks[4] = {SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT};

/* Global sleep queue for tasks blocked on time */
static struct task* sched_sleep_head = NULL;
static spinlock_t sched_sleep_lock = SPINLOCK_INIT;

/* Housekeeping state for task cleanup */
static struct task* sched_cleanup_tasks[4] = {NULL};
static int sched_core_pids[4] = {-1, -1, -1, -1};

/* Global task ID generation */
static int sched_next_id = 0;
static spinlock_t sched_next_id_lock = SPINLOCK_INIT;

void sched_check_corruption(void)
{
    if (sched_canary_before != 0xAAAAAAAAAAAAAAAAULL || sched_canary_after != 0xBBBBBBBBBBBBBBBBULL)
    {
        printf("SCHED: Memory corruption detected around sched_init_tasks!\n");
        printf("  Before: %p (expected 0xAAAAAAAAAAAAAAAA)\n", (void*)sched_canary_before);
        printf("  After:  %p (expected 0xBBBBBBBBBBBBBBBB)\n", (void*)sched_canary_after);
        PANIC("Memory corruption");
    }
}

/*
 * get_core_id - Returns the index of the current CPU core (0-3).
 */
static inline int get_core_id(void)
{
    unsigned long mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (int)(mpidr & 3);
}

/*
 * enqueue_ready - Appends a task to the end of a specific CPU's ready queue.
 */
static void enqueue_ready(int cpu, struct task* t)
{
    if (!t)
        return;
    unsigned long flags = spin_lock_irqsave(&sched_ready_locks[cpu]);

    t->state = SCHED_TASK_READY;
    t->next = NULL;

    if (!sched_ready_heads[cpu])
    {
        sched_ready_heads[cpu] = t;
        sched_ready_tails[cpu] = t;
    }
    else
    {
        sched_ready_tails[cpu]->next = t;
        sched_ready_tails[cpu] = t;
    }

    spin_unlock_irqrestore(&sched_ready_locks[cpu], flags);
}

/*
 * dequeue_ready - Removes and returns the first task from a CPU's ready queue.
 * If 'allow_pid0' is 0, it skips tasks with PID 0 (system/boot tasks).
 */
static struct task* dequeue_ready_filtered(int cpu, int allow_pid0)
{
    unsigned long flags = spin_lock_irqsave(&sched_ready_locks[cpu]);

    struct task* prev = NULL;
    struct task* curr = sched_ready_heads[cpu];

    while (curr)
    {
        if (allow_pid0 || curr->pid != 0)
        {
            if (prev)
                prev->next = curr->next;
            else
                sched_ready_heads[cpu] = curr->next;

            if (!sched_ready_heads[cpu])
                sched_ready_tails[cpu] = NULL;
            else if (curr == sched_ready_tails[cpu])
                sched_ready_tails[cpu] = prev;

            curr->next = NULL;
            spin_unlock_irqrestore(&sched_ready_locks[cpu], flags);
            return curr;
        }
        prev = curr;
        curr = curr->next;
    }

    spin_unlock_irqrestore(&sched_ready_locks[cpu], flags);
    return NULL;
}

static struct task* dequeue_ready(int cpu)
{
    return dequeue_ready_filtered(cpu, 1);
}

/*
 * insert_sleep - Inserts a task into the global sleep queue, ordered by wake time.
 */
static void insert_sleep(struct task* t)
{
    unsigned long flags = spin_lock_irqsave(&sched_sleep_lock);

    if (!sched_sleep_head || t->wake_time < sched_sleep_head->wake_time)
    {
        t->next = sched_sleep_head;
        sched_sleep_head = t;
        spin_unlock_irqrestore(&sched_sleep_lock, flags);
        return;
    }

    struct task* curr = sched_sleep_head;
    while (curr->next && curr->next->wake_time <= t->wake_time)
    {
        curr = curr->next;
    }

    t->next = curr->next;
    curr->next = t;
    spin_unlock_irqrestore(&sched_sleep_lock, flags);
}

/* Low-level assembly wrapper for initial task entry */
extern void task_wrapper_asm(void);

/*
 * idle_task_entry - The main loop for CPU idle tasks.
 */
static void idle_task_entry(void)
{
    while (1)
    {
        asm volatile("wfe");
    }
}

/*
 * create_idle_task - Allocates and initializes an idle task for a specific core.
 */
static struct task* create_idle_task(int id)
{
    struct task* idle = (struct task*)heap_malloc(sizeof(struct task));
    memset(idle, 0, sizeof(struct task));

    unsigned char* stack = (unsigned char*)pmm_alloc_pages(SCHED_STACK_PAGES);
    mmu_unmap_page((unsigned long)stack); /* Guard page */

    unsigned char* usable = stack + PAGE_SIZE;
    unsigned long sp = ((unsigned long)usable + SCHED_TASK_STACK_SIZE) & ~15UL;

    idle->state = SCHED_TASK_READY;
    idle->id = 900 + id;
    idle->pid = 0;
    idle->stack = stack;
    idle->ttbr0 = mmu_kernel_ttbr0();
    idle->context.sp = sp;
    idle->context.lr = (unsigned long)task_wrapper_asm;
    idle->context.x19 = (unsigned long)idle_task_entry;

    *SCHED_TASK_CANARY_PTR(idle) = SCHED_STACK_CANARY;

    return idle;
}

/*
 * sched_init - Primary core scheduler initialization.
 */
void sched_init(void)
{
    struct task* main_task = &sched_init_tasks[0];
    memset(main_task, 0, sizeof(struct task));

    main_task->state = SCHED_TASK_RUNNING;
    main_task->id = sched_next_id++;
    main_task->pid = 0;
    main_task->stack = NULL;
    main_task->ttbr0 = mmu_kernel_ttbr0();

    /* Store the task pointer in TPIDR_EL1 for fast retrieval */
    asm volatile("msr tpidr_el1, %0" : : "r"(main_task));
    sched_idle_tasks[0] = create_idle_task(0);

    printf("[ SCHED ] Scheduler initialized on CPU0\n");
}

/*
 * sched_secondary_init - Secondary core scheduler initialization.
 */
void sched_secondary_init(void)
{
    int core_id = get_core_id();

    struct task* boot_task = &sched_init_tasks[core_id];
    memset(boot_task, 0, sizeof(struct task));

    boot_task->state = SCHED_TASK_DEAD;
    boot_task->id = 800 + core_id;
    boot_task->pid = 0;
    boot_task->ttbr0 = mmu_kernel_ttbr0();

    asm volatile("msr tpidr_el1, %0" : : "r"(boot_task));
    sched_idle_tasks[core_id] = create_idle_task(core_id);

    schedule();
}

/*
 * sched_create_task - Spawns a new kernel thread.
 */
void sched_create_task(void (*entry)(void))
{
    struct task* t = (struct task*)heap_malloc(sizeof(struct task));
    memset(t, 0, sizeof(struct task));

    unsigned char* stack = (unsigned char*)pmm_alloc_pages(SCHED_STACK_PAGES);
    mmu_unmap_page((unsigned long)stack);

    unsigned char* usable = stack + PAGE_SIZE;
    unsigned long sp = ((unsigned long)usable + SCHED_TASK_STACK_SIZE) & ~15UL;

    t->state = SCHED_TASK_READY;
    t->context.sp = sp;
    t->context.lr = (unsigned long)task_wrapper_asm;
    t->context.x19 = (unsigned long)entry;
    t->ttbr0 = mmu_kernel_ttbr0();
    t->pid = 0;
    t->stack = stack;

    *SCHED_TASK_CANARY_PTR(t) = SCHED_STACK_CANARY;

    unsigned long flags = spin_lock_irqsave(&sched_next_id_lock);
    t->id = sched_next_id++;
    spin_unlock_irqrestore(&sched_next_id_lock, flags);

    enqueue_ready(get_core_id(), t);
}

/*
 * sched_sleep_ms - Blocks the current task for the specified duration.
 */
void sched_sleep_ms(unsigned long ms)
{
    unsigned long flags = irq_save();
    struct task* curr = sched_get_current();
    int cpu = get_core_id();

    if (!curr || curr == sched_idle_tasks[cpu])
    {
        irq_restore(flags);
        return;
    }

    curr->state = SCHED_TASK_BLOCKED;
    curr->wake_time = get_system_time() + ms;
    insert_sleep(curr);

    schedule();
    irq_restore(flags);
}

/*
 * sched_create_user_task - Initializes a task for a user process.
 */
struct task* sched_create_user_task(unsigned long forged_sp, unsigned long forged_lr, uint32_t pid)
{
    struct task* t = (struct task*)heap_malloc(sizeof(struct task));
    memset(t, 0, sizeof(struct task));

    t->state = SCHED_TASK_READY;
    t->context.sp = forged_sp;
    t->context.lr = forged_lr;
    t->stack = (unsigned char*)(process_table[pid].vaddr_kernel_stack - PAGE_SIZE);
    t->ttbr0 = process_get_ttbr0(pid);
    t->pid = pid;

    unsigned long flags = spin_lock_irqsave(&sched_next_id_lock);
    t->id = sched_next_id++;
    spin_unlock_irqrestore(&sched_next_id_lock, flags);

    enqueue_ready(get_core_id(), t);
    return t;
}

/*
 * sched_block - Voluntary yield to the blocked state.
 */
void sched_block(void)
{
    unsigned long flags = irq_save();
    struct task* curr = sched_get_current();

    if (!curr || curr == sched_idle_tasks[get_core_id()])
    {
        irq_restore(flags);
        return;
    }

    curr->state = SCHED_TASK_BLOCKED;
    schedule();
    irq_restore(flags);
}

/*
 * sched_unblock - Transitions a task back to the ready queue.
 */
void sched_unblock(struct task* t)
{
    if (!t)
        return;

    enum sched_task_state expected = SCHED_TASK_BLOCKED;
    if (__atomic_compare_exchange_n(&t->state, &expected, SCHED_TASK_READY, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
        enqueue_ready(get_core_id(), t);
    }
}

/*
 * sched_get_current - Retrieves the current core's task from TPIDR_EL1.
 */
struct task* sched_get_current(void)
{
    struct task* t;
    asm volatile("mrs %0, tpidr_el1" : "=r"(t));
    return t;
}

/*
 * sched_get_core_pid - Returns the PID of the process on a given core.
 */
int sched_get_core_pid(int cpu)
{
    if (cpu < 0 || cpu >= 4)
    {
        return -PERS_ERR_INVALID_ARGUMENT;
    }
    return sched_core_pids[cpu];
}

/*
 * schedule - Main scheduling routine and context switch logic.
 */
void schedule(void)
{
    unsigned long flags = irq_save();
    int cpu = get_core_id();

    sched_check_corruption();

    /* 1. Cleanup tasks marked for deletion on this core */
    struct task* dead = sched_cleanup_tasks[cpu];
    if (dead)
    {
        struct task* curr = sched_get_current();
        if (curr == dead)
        {
            PANIC("SCHED: Attempted to cleanup the active task");
        }

        sched_cleanup_tasks[cpu] = NULL;

        if (dead->pid != 0)
        {
            process_exit(dead->pid, process_table[dead->pid].exit_status);
        }

        if (dead->stack)
        {
            unsigned long guard_va = (unsigned long)dead->stack;
            mmu_map_page(guard_va, V2P(guard_va), MMU_FLAGS_KERNEL_RW);
            pmm_free_pages(dead->stack, SCHED_STACK_PAGES);
        }

        if (dead < &sched_init_tasks[0] || dead > &sched_init_tasks[3])
        {
            heap_free(dead);
        }
    }

    /* 2. Wake up tasks from the sleep queue */
    unsigned long now = get_system_time();
    unsigned long s_flags = spin_lock_irqsave(&sched_sleep_lock);
    while (sched_sleep_head && sched_sleep_head->wake_time <= now)
    {
        struct task* w = sched_sleep_head;
        sched_sleep_head = w->next;
        w->state = SCHED_TASK_READY;
        enqueue_ready(cpu, w);
    }
    spin_unlock_irqrestore(&sched_sleep_lock, s_flags);

    struct task* prev = sched_get_current();
    if (!prev)
    {
        irq_restore(flags);
        return;
    }

    /* 3. Check for stack overflow using the canary */
    if (prev->stack && *SCHED_TASK_CANARY_PTR(prev) != SCHED_STACK_CANARY)
    {
        PANIC("SCHED: Stack overflow detected via canary");
    }

    /* 4. Update state of the outgoing task */
    if (prev->state == SCHED_TASK_RUNNING && prev != sched_idle_tasks[cpu])
    {
        prev->state = SCHED_TASK_READY;
        enqueue_ready(cpu, prev);
    }
    else if (prev->state == SCHED_TASK_DEAD)
    {
        sched_cleanup_tasks[cpu] = prev;
    }

    /* 5. Select the next task to execute */
    struct task* next = dequeue_ready(cpu);
    if (!next)
    {
        /* Attempt to steal work from other cores if local queue is empty */
        for (int i = 0; i < 4; i++)
        {
            if (i == cpu)
                continue;
            /* Do NOT steal PID 0 (boot/system) tasks from other cores */
            next = dequeue_ready_filtered(i, 0);
            if (next)
                break;
        }
    }

    if (!next)
    {
        next = sched_idle_tasks[cpu];
    }

    /* 6. Perform the hardware context switch */
    next->state = SCHED_TASK_RUNNING;
    sched_core_pids[cpu] = (int)next->pid;

    asm volatile("msr tpidr_el1, %0" : : "r"(next));
    asm volatile("msr ttbr0_el1, %0" : : "r"(next->ttbr0));
    asm volatile("dsb sy");
    asm volatile("isb");

    if (prev != next)
    {
        /* Safety check for corrupted task context before switching */
        if (next->context.lr == 0 || next->context.sp == 0)
        {
            printf("SCHED: Fatal error - Task %lu (PID %d) has corrupted context (LR=0x%lx, SP=0x%lx)!\n",
                   next->id,
                   (int)next->pid,
                   next->context.lr,
                   next->context.sp);
            PANIC("Scheduler context corruption");
        }
        switch_context(&prev->context, &next->context);
    }

    irq_restore(flags);
}
