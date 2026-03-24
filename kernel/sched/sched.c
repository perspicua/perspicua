/*
 * kernel/sched/sched.c
 *
 * Preemptive, SMP-aware round-robin scheduler for AArch64.
 *
 * ── Design overview ──────────────────────────────────────────────────────────
 *
 *  Per-CPU ready queues
 *    Each core owns a singly-linked FIFO of READY tasks protected by its own
 *    spinlock.  Tasks are appended at the tail and removed from the head so
 *    that every runnable task gets a fair turn.
 *
 *  Global sleep queue
 *    A single list ordered by wake_time, protected by sched_sleep_lock.
 *    schedule() drains any tasks whose deadline has passed back into the
 *    local ready queue before selecting the next task to run.
 *
 *  Work-stealing
 *    When a core's ready queue is empty it scans the other cores' queues and
 *    steals the first eligible task it finds.  PID-0 kernel/boot tasks are
 *    never stolen.
 *
 *  Deferred dead-task cleanup
 *    A task cannot free its own stack while it is still executing on it.
 *    When a task transitions to SCHED_TASK_DEAD, schedule() stashes it in
 *    sched_cleanup[cpu] and the *next* call to schedule() on that core
 *    actually releases the memory.
 *
 *  Current-task pointer
 *    TPIDR_EL1 holds a pointer to the struct task of the currently running
 *    task on each core.  sched_get_current() is therefore a single MRS
 *    instruction.
 *
 * ── Lock ordering ────────────────────────────────────────────────────────────
 *
 *   sched_sleep_lock          (global, coarse)
 *   sched_ready_locks[cpu]    (per-CPU, fine)
 *   sched_next_id_lock        (global, narrow — only held for an increment)
 *
 *   Never acquire a ready-queue lock while holding the sleep lock, or
 *   vice-versa.  The cleanup slot and idle-task pointers are only ever
 *   accessed from the owning core, so they need no lock.
 *
 * ── IRQ discipline ───────────────────────────────────────────────────────────
 *
 *   schedule() and every public function that touches shared state disables
 *   IRQs for its critical section via irq_save()/irq_restore().  Individual
 *   spinlock helpers (spin_lock_irqsave) are used for queue operations so
 *   that an interrupt arriving mid-operation cannot deadlock on a lock that
 *   the interrupted code already holds.
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

#include "types.h"
/* --------------------------------------------------------------------------
 * Compile-time tunables
 * -------------------------------------------------------------------------- */

#ifndef SCHED_NUM_CORES
    #define SCHED_NUM_CORES 4
#endif

/* --------------------------------------------------------------------------
 * Static assertions — keep in sync with switch.S offsets
 * -------------------------------------------------------------------------- */

_Static_assert(sizeof(struct cpu_context) == 104, "cpu_context size mismatch — update switch.S");
_Static_assert(__builtin_offsetof(struct task, state) == 112, "task->state offset mismatch — update task_wrapper_asm");

/* --------------------------------------------------------------------------
 * Internal macros
 * -------------------------------------------------------------------------- */

/* Address of the stack-canary word at the bottom of the usable stack region
 * (immediately above the guard page). */
#define TASK_CANARY_PTR(t) ((unsigned long*)((t)->stack + PAGE_SIZE))

/* --------------------------------------------------------------------------
 * Static scheduler state
 * --------------------------------------------------------------------------
 *
 * The canary sentinels around sched_boot_tasks[] detect wild writes that
 * overflow into the scheduler's own BSS.
 */

/* Boot/init task structs — one per core, statically allocated so that
 * core 0's initial task does not need heap_malloc before the heap exists. */
static uint64_t s_canary_lo = 0xAAAAAAAAAAAAAAAAULL;
static struct task sched_boot_tasks[SCHED_NUM_CORES];
static uint64_t s_canary_hi = 0xBBBBBBBBBBBBBBBBULL;

/* One idle task per core, allocated during sched_init / sched_secondary_init. */
static struct task* sched_idle[SCHED_NUM_CORES];

/* Per-CPU ready queues. */
static struct task* sched_rq_head[SCHED_NUM_CORES];
static struct task* sched_rq_tail[SCHED_NUM_CORES];
static spinlock_t sched_rq_lock[SCHED_NUM_CORES] = {[0 ... SCHED_NUM_CORES - 1] = SPINLOCK_INIT};

/* Global sleep queue (ordered by wake_time, earliest first). */
static struct task* sched_sleep_head;
static spinlock_t sched_sleep_lock = SPINLOCK_INIT;

/* Per-core deferred-cleanup slot: task to be freed on the next schedule(). */
static struct task* sched_cleanup[SCHED_NUM_CORES];

/* Per-core PID tracking (for sched_get_core_pid). */
static int sched_core_pid[SCHED_NUM_CORES];

/* Monotonically increasing task ID. */
static unsigned long sched_next_id;
static spinlock_t sched_id_lock = SPINLOCK_INIT;

/* --------------------------------------------------------------------------
 * Forward declarations (assembly)
 * -------------------------------------------------------------------------- */

/* Defined in switch.S.  New tasks start here; x19 holds the entry point. */
extern void task_wrapper_asm(void);

/* --------------------------------------------------------------------------
 * Internal helpers — canary / corruption checks
 * -------------------------------------------------------------------------- */

/*
 * sched_check_bss_canaries — verify the sentinels around sched_boot_tasks[].
 * Called at the top of every schedule() invocation.
 */
static void sched_check_bss_canaries(void)
{
    if (s_canary_lo != 0xAAAAAAAAAAAAAAAAULL || s_canary_hi != 0xBBBBBBBBBBBBBBBBULL)
    {
        printf("SCHED: BSS canary corruption!\n");
        printf("  lo: 0x%llx (expected 0xAAAAAAAAAAAAAAAA)\n", (unsigned long long)s_canary_lo);
        printf("  hi: 0x%llx (expected 0xBBBBBBBBBBBBBBBB)\n", (unsigned long long)s_canary_hi);
        PANIC("sched: memory corruption around boot task structs");
    }
}

/*
 * task_check_stack_canary — verify that a task's kernel stack has not
 * overflowed into the guard page region.
 */
static void task_check_stack_canary(const struct task* t)
{
    if (t->stack && *TASK_CANARY_PTR(t) != SCHED_STACK_CANARY)
    {
        printf("SCHED: stack overflow in task id=%lu pid=%u\n", t->id, (unsigned)t->pid);
        PANIC("sched: stack overflow detected via canary");
    }
}

/* --------------------------------------------------------------------------
 * Internal helpers — task ID
 * -------------------------------------------------------------------------- */

static unsigned long alloc_task_id(void)
{
    unsigned long flags = spin_lock_irqsave(&sched_id_lock);
    unsigned long id = sched_next_id++;
    spin_unlock_irqrestore(&sched_id_lock, flags);
    return id;
}

/* --------------------------------------------------------------------------
 * Internal helpers — kernel stack allocation
 * -------------------------------------------------------------------------- */

/*
 * alloc_task_stack — allocate SCHED_STACK_PAGES physical pages, unmap the
 * first page as a guard, write the canary at the bottom of the usable region,
 * and return a pointer to the raw allocation base (i.e. the guard page).
 *
 * The returned pointer is stored in task->stack so that TASK_CANARY_PTR()
 * and the cleanup path (pmm_free_pages + re-map guard) work correctly.
 *
 * Returns NULL on OOM.
 */
static unsigned char* alloc_task_stack(void)
{
    unsigned char* base = (unsigned char*)pmm_alloc_pages(SCHED_STACK_PAGES);
    if (!base)
        return NULL;

    mmu_unmap_page((unsigned long)base); /* guard page */
    *(unsigned long*)(base + PAGE_SIZE) = SCHED_STACK_CANARY;
    return base;
}

/*
 * free_task_stack — release a stack previously obtained with alloc_task_stack.
 * Re-maps the guard page before handing memory back to the PMM so that the
 * physical page is clean for reuse.
 */
static void free_task_stack(unsigned char* stack)
{
    if (!stack)
        return;

    /*
     * Sanity check: stack must be a kernel virtual address.
     * If it's below KERNEL_VMA it's a physical address — someone corrupted
     * dead->stack after sched_create_user_task wrote it.
     */
    if ((unsigned long)stack < KERNEL_VMA)
    {
        printf("SCHED: free_task_stack called with non-VA pointer: 0x%lx\n", (unsigned long)stack);
        printf("SCHED: this is a physical address — t->stack was corrupted\n");
        PANIC("sched: corrupted t->stack in free_task_stack");
    }

    mmu_map_page((unsigned long)stack, V2P(stack), MMU_FLAGS_KERNEL_RW);
    pmm_free_pages(stack, SCHED_STACK_PAGES);
}
/*
 * init_task_stack_context — set up the cpu_context fields for a brand-new
 * task so that the first switch_context() into it lands in task_wrapper_asm
 * with x19 = entry_fn.
 *
 * stack  — the raw base pointer returned by alloc_task_stack()
 * entry  — the C function the task should execute
 */
static void init_task_stack_context(struct task* t, unsigned char* stack, void (*entry)(void))
{
    unsigned long sp = ((unsigned long)(stack + PAGE_SIZE) + SCHED_TASK_STACK_SIZE) & ~15UL;
    t->stack = stack;
    t->context.sp = sp;
    t->context.lr = (unsigned long)task_wrapper_asm;
    t->context.x19 = (unsigned long)entry;
}

/* --------------------------------------------------------------------------
 * Internal helpers — ready queue
 * -------------------------------------------------------------------------- */

/*
 * rq_enqueue — append a task to the tail of cpu's ready queue.
 * Caller must NOT hold sched_rq_lock[cpu].
 */
static void rq_enqueue(int cpu, struct task* t)
{
    unsigned long flags = spin_lock_irqsave(&sched_rq_lock[cpu]);

    t->state = SCHED_TASK_READY;
    t->next = NULL;

    if (!sched_rq_head[cpu])
    {
        sched_rq_head[cpu] = t;
        sched_rq_tail[cpu] = t;
    }
    else
    {
        sched_rq_tail[cpu]->next = t;
        sched_rq_tail[cpu] = t;
    }

    spin_unlock_irqrestore(&sched_rq_lock[cpu], flags);
}

/*
 * rq_dequeue — remove and return the first eligible task from cpu's queue.
 *
 * If allow_pid0 is 0, tasks with pid == 0 are skipped (used by work-stealing
 * to avoid migrating boot/kernel tasks off their home core).
 *
 * Returns NULL if no eligible task is found.
 */
static struct task* rq_dequeue(int cpu, int allow_pid0)
{
    unsigned long flags = spin_lock_irqsave(&sched_rq_lock[cpu]);

    struct task* prev = NULL;
    struct task* curr = sched_rq_head[cpu];

    while (curr)
    {
        if (allow_pid0 || curr->pid != 0)
        {
            /* Unlink curr from the list. */
            if (prev)
                prev->next = curr->next;
            else
                sched_rq_head[cpu] = curr->next;

            if (curr == sched_rq_tail[cpu])
                sched_rq_tail[cpu] = prev;

            /* Tail must be NULL when queue is empty. */
            if (!sched_rq_head[cpu])
                sched_rq_tail[cpu] = NULL;

            curr->next = NULL;
            spin_unlock_irqrestore(&sched_rq_lock[cpu], flags);
            return curr;
        }
        prev = curr;
        curr = curr->next;
    }

    spin_unlock_irqrestore(&sched_rq_lock[cpu], flags);
    return NULL;
}

/* --------------------------------------------------------------------------
 * Internal helpers — sleep queue
 * -------------------------------------------------------------------------- */

/*
 * sleep_enqueue — insert a task into the global sleep queue in wake_time
 * order (earliest first).  Uses signed difference to handle wrap-around.
 */
static void sleep_enqueue(struct task* t)
{
    unsigned long flags = spin_lock_irqsave(&sched_sleep_lock);

    if (!sched_sleep_head || (long)(t->wake_time - sched_sleep_head->wake_time) < 0)
    {
        t->next = sched_sleep_head;
        sched_sleep_head = t;
        spin_unlock_irqrestore(&sched_sleep_lock, flags);
        return;
    }

    struct task* curr = sched_sleep_head;
    while (curr->next && (long)(t->wake_time - curr->next->wake_time) >= 0)
        curr = curr->next;

    t->next = curr->next;
    curr->next = t;

    spin_unlock_irqrestore(&sched_sleep_lock, flags);
}

/*
 * sleep_drain — move all tasks whose wake_time has passed from the sleep
 * queue back into 'cpu's ready queue.  Uses an atomic CAS to guard against
 * the rare case where sched_unblock() races with the timer expiry.
 */
static void sleep_drain(int cpu)
{
    unsigned long now = get_system_time();
    unsigned long flags = spin_lock_irqsave(&sched_sleep_lock);

    while (sched_sleep_head && (long)(now - sched_sleep_head->wake_time) >= 0)
    {
        struct task* w = sched_sleep_head;
        sched_sleep_head = w->next;
        w->next = NULL;
        spin_unlock_irqrestore(&sched_sleep_lock, flags);

        /* CAS: only enqueue if still BLOCKED; sched_unblock() may have
         * already moved the task to READY via a different code path. */
        enum sched_task_state expected = SCHED_TASK_BLOCKED;
        if (__atomic_compare_exchange_n(&w->state,
                                        &expected,
                                        SCHED_TASK_READY,
                                        /*weak=*/0,
                                        __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST))
            rq_enqueue(cpu, w);

        flags = spin_lock_irqsave(&sched_sleep_lock);
    }

    spin_unlock_irqrestore(&sched_sleep_lock, flags);
}

/* --------------------------------------------------------------------------
 * Internal helpers — idle task
 * -------------------------------------------------------------------------- */

static void idle_entry(void)
{
    enable_interrupts();
    for (;;)
        asm volatile("wfe");
}

static struct task* create_idle_task(int core_id)
{
    struct task* t = (struct task*)heap_malloc(sizeof(struct task));
    if (!t)
        PANIC("sched: OOM allocating idle task");
    memset(t, 0, sizeof(*t));

    unsigned char* stack = alloc_task_stack();
    if (!stack)
        PANIC("sched: OOM allocating idle task stack");

    init_task_stack_context(t, stack, idle_entry);

    t->state = SCHED_TASK_READY;
    t->id = 900 + (unsigned long)core_id;
    t->pid = 0;
    t->ttbr0 = mmu_kernel_ttbr0();

    return t;
}

/* --------------------------------------------------------------------------
 * Internal helpers — deferred dead-task cleanup
 * -------------------------------------------------------------------------- */

/*
 * cleanup_dead_task — free the stack and (if heap-allocated) the task struct
 * of a task that died on a previous schedule() invocation on this core.
 *
 * Must be called before prev/next selection so we never free the task we are
 * about to switch into.
 *
 * sched_boot_tasks[] are statically allocated and must not be passed to
 * heap_free().
 */
static void cleanup_dead_task(int cpu)
{
    struct task* dead = sched_cleanup[cpu];
    if (!dead)
        return;

    if (dead == sched_get_current())
        PANIC("sched: attempt to free the active task");

    sched_cleanup[cpu] = NULL;

    /*
     * Validate the task struct before using any of its fields.
     * Dump everything so we can identify the corruption source.
     */
    printf("SCHED: cleanup task id=%lu pid=%u stack=0x%lx ttbr0=0x%lx state=%d\n",
           dead->id,
           (unsigned)dead->pid,
           (unsigned long)dead->stack,
           dead->ttbr0,
           (int)dead->state);

    if ((unsigned long)dead->stack != 0 && (unsigned long)dead->stack < KERNEL_VMA)
    {
        printf("SCHED: CORRUPTION DETECTED\n");
        printf("  dead->stack = 0x%lx (physical address — should be kernel VA)\n", (unsigned long)dead->stack);
        printf("  dead->id    = %lu\n", dead->id);
        printf("  dead->pid   = %u\n", (unsigned)dead->pid);
        printf("  dead addr   = 0x%lx\n", (unsigned long)dead);
        /*
         * Print what's at offset 144 in raw hex — this is the corrupted
         * t->stack field. Bytes 136-160 of the task struct:
         */
        unsigned char* raw = (unsigned char*)dead;
        printf("  task bytes [136..167]: ");
        for (int i = 136; i < 168; i++)
            printf("%02x ", raw[i]);
        printf("\n");
        /*
         * Also check if 'dead' itself looks like a valid heap pointer.
         * If dead < KERNEL_VMA then the task struct pointer is also corrupted.
         */
        if ((unsigned long)dead < KERNEL_VMA)
        {
            printf("  dead pointer itself is not a kernel VA!\n");
        }
        PANIC("sched: t->stack corrupted before cleanup_dead_task");
    }

    free_task_stack(dead->stack);
    dead->stack = NULL;

    int is_boot_task = (dead >= &sched_boot_tasks[0] && dead < &sched_boot_tasks[SCHED_NUM_CORES]);
    if (!is_boot_task)
        heap_free(dead);
}
/* --------------------------------------------------------------------------
 * Public API — enqueue_ready (called from process.c and elsewhere)
 * -------------------------------------------------------------------------- */

void enqueue_ready(int cpu, struct task* t)
{
    if (t && t->stack && (unsigned long)t->stack < KERNEL_VMA)
    {
        printf("SCHED: enqueue_ready: t->stack=0x%lx is not kernel VA! "
               "id=%lu pid=%u\n",
               (unsigned long)t->stack,
               t->id,
               (unsigned)t->pid);
        PANIC("sched: corrupted t->stack at enqueue");
    }
    rq_enqueue(cpu, t);
}

/* --------------------------------------------------------------------------
 * Public API — initialisation
 * -------------------------------------------------------------------------- */

/*
 * sched_init — initialise the scheduler on the primary core (CPU 0).
 *
 * Called once during boot before any secondary cores are started.  The boot
 * task is a static struct so that this can run before the heap exists.
 */
void sched_init(void)
{
    struct task* boot = &sched_boot_tasks[0];
    memset(boot, 0, sizeof(*boot));

    boot->state = SCHED_TASK_RUNNING;
    boot->id = alloc_task_id();
    boot->pid = 0;
    boot->stack = NULL; /* boot task has no separately allocated stack */
    boot->ttbr0 = mmu_kernel_ttbr0();

    /* Publish as the current task for CPU 0. */
    asm volatile("msr tpidr_el1, %0" ::"r"(boot));

    sched_idle[0] = create_idle_task(0);
    sched_core_pid[0] = 0;

    printf("[ SCHED ] Scheduler initialized (SCHED_NUM_CORES=%d)\n", SCHED_NUM_CORES);
}

/*
 * sched_secondary_init — initialise the scheduler on a secondary core.
 *
 * Each secondary core calls this after the MMU and stack are up.  The boot
 * task is immediately marked DEAD (it will be cleaned up on the first
 * schedule()) and the core dives straight into the scheduling loop.
 */
void sched_secondary_init(void)
{
    int core_id = get_core_id();

    struct task* boot = &sched_boot_tasks[core_id];
    memset(boot, 0, sizeof(*boot));

    boot->state = SCHED_TASK_DEAD;
    boot->id = 800 + (unsigned long)core_id;
    boot->pid = 0;
    boot->ttbr0 = mmu_kernel_ttbr0();

    asm volatile("msr tpidr_el1, %0" ::"r"(boot));

    sched_idle[core_id] = create_idle_task(core_id);
    sched_core_pid[core_id] = 0;

    enable_interrupts();
    schedule();

    /* Unreachable — schedule() never returns on a secondary core after init. */
    PANIC("sched_secondary_init: schedule() returned unexpectedly");
}

/* --------------------------------------------------------------------------
 * Public API — task creation
 * -------------------------------------------------------------------------- */

/*
 * sched_create_task — spawn a new kernel thread starting at 'entry'.
 *
 * The new task is immediately placed on the local core's ready queue.
 */
void sched_create_task(void (*entry)(void))
{
    struct task* t = (struct task*)heap_malloc(sizeof(struct task));
    if (!t)
        PANIC("sched_create_task: OOM for task struct");
    memset(t, 0, sizeof(*t));

    unsigned char* stack = alloc_task_stack();
    if (!stack)
        PANIC("sched_create_task: OOM for stack");

    init_task_stack_context(t, stack, entry);

    t->state = SCHED_TASK_READY;
    t->ttbr0 = mmu_kernel_ttbr0();
    t->pid = 0;
    t->id = alloc_task_id();

    rq_enqueue(get_core_id(), t);
}

/*
 * sched_create_user_task — allocate and initialise a task struct for a
 * user-mode process.
 *
 * The caller (process.c) has already set up the kernel stack and forged the
 * trap frame; it passes the pre-computed sp and lr values directly.
 *
 * Returns the new task pointer, or NULL on OOM.
 */
struct task*
sched_create_user_task(unsigned long forged_sp, unsigned long forged_lr, uintptr_t kstack_base, uint32_t pid)
{
    struct task* t = (struct task*)heap_malloc(sizeof(struct task));
    if (!t)
        return NULL;
    memset(t, 0, sizeof(*t));

    t->state = SCHED_TASK_READY;
    t->context.sp = forged_sp;
    t->context.lr = forged_lr;
    t->pid = pid;
    t->id = alloc_task_id();

    /*
     * t->stack points to the guard page base of the kernel stack so that
     * TASK_CANARY_PTR() resolves correctly and free_task_stack() can release
     * the allocation.  kstack_base is the first *usable* byte (guard page + 1),
     * so we step back by PAGE_SIZE to find the actual allocation base.
     */
    t->stack = (unsigned char*)(kstack_base - PAGE_SIZE);
    t->ttbr0 = process_get_ttbr0(pid);
    // printf("SCHED: created user task id=%lu pid=%u stack=0x%lx\n", t->id, (unsigned)t->pid, (unsigned long)t->stack);
    return t;
}

/* --------------------------------------------------------------------------
 * Public API — blocking / sleeping
 * -------------------------------------------------------------------------- */

/*
 * sched_sleep_ms — put the current task to sleep for at least 'ms' ms.
 *
 * The idle task cannot sleep; the call is silently ignored in that case.
 */
void sched_sleep_ms(unsigned long ms)
{
    unsigned long flags = irq_save();
    int cpu = get_core_id();
    struct task* curr = sched_get_current();

    if (!curr || curr == sched_idle[cpu])
    {
        irq_restore(flags);
        return;
    }

    curr->state = SCHED_TASK_BLOCKED;
    curr->wake_time = get_system_time() + ms;
    sleep_enqueue(curr);

    schedule();
    irq_restore(flags);
}

/*
 * sched_block — voluntarily yield to the BLOCKED state.
 *
 * The caller is responsible for arranging a future sched_unblock() (or a
 * sleep-queue wake-up) so the task is not lost.  The idle task cannot block.
 */
void sched_block(void)
{
    unsigned long flags = irq_save();
    int cpu = get_core_id();
    struct task* curr = sched_get_current();

    if (!curr || curr == sched_idle[cpu])
    {
        irq_restore(flags);
        return;
    }

    curr->state = SCHED_TASK_BLOCKED;
    schedule();
    irq_restore(flags);
}

/*
 * sched_unblock — move a blocked task back onto a ready queue.
 *
 * Uses a CAS so that concurrent calls (e.g. from an IRQ handler and a
 * timer expiry) are idempotent.  The task is always placed on the local
 * core's queue; future work could prefer the task's home core.
 */
void sched_unblock(struct task* t)
{
    if (!t)
        return;

    /* If it's already READY, it's already in the queue or running. */
    if (__atomic_load_n(&t->state, __ATOMIC_SEQ_CST) == SCHED_TASK_READY)
        return;

    enum sched_task_state expected = SCHED_TASK_BLOCKED;
    if (__atomic_compare_exchange_n(&t->state,
                                    &expected,
                                    SCHED_TASK_READY,
                                    /*weak=*/0,
                                    __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST))
        rq_enqueue(get_core_id(), t);
}

/* --------------------------------------------------------------------------
 * Public API — introspection
 * -------------------------------------------------------------------------- */

struct task* sched_get_current(void)
{
    struct task* t;
    asm volatile("mrs %0, tpidr_el1" : "=r"(t));
    return t;
}

int sched_get_core_pid(int cpu)
{
    if (cpu < 0 || cpu >= SCHED_NUM_CORES)
        return -PERS_ERR_INVALID_ARGUMENT;
    return sched_core_pid[cpu];
}

/* --------------------------------------------------------------------------
 * Public API — schedule()
 * -------------------------------------------------------------------------- */

/*
 * schedule — select the next task and switch to it.
 *
 * Steps (in order):
 *   1. Free any dead task left over from the previous invocation.
 *   2. Drain expired sleepers back into the ready queue.
 *   3. Validate the outgoing task's stack canary.
 *   4. Transition the outgoing task: RUNNING -> READY (re-enqueue) or
 *      DEAD -> stash in cleanup slot.
 *   5. Select the next task: local queue -> work-steal -> idle.
 *   6. Context switch.
 *
 * IRQs are disabled for the duration.  switch_context() saves callee-saved
 * registers of 'prev' and restores those of 'next'; when prev is eventually
 * resumed it returns from switch_context() and proceeds to irq_restore().
 */
void schedule(void)
{
    unsigned long flags = irq_save();
    int cpu = get_core_id();

    /* ── Step 1: deferred dead-task cleanup ── */
    cleanup_dead_task(cpu);

    /* ── Step 2: wake expired sleepers ── */
    sleep_drain(cpu);

    /* ── Step 3: sanity-check BSS sentinels ── */
    sched_check_bss_canaries();

    struct task* prev = sched_get_current();
    if (!prev)
    {
        /* Should never happen after sched_init, but be defensive. */
        irq_restore(flags);
        return;
    }

    /* ── Step 3b: stack canary check on the outgoing task ── */
    task_check_stack_canary(prev);

    /* ── Step 4: transition outgoing task ── */
    switch (prev->state)
    {
    case SCHED_TASK_RUNNING:
        /*
         * Normal preemption or voluntary yield — put the task back at the
         * tail of the ready queue so other tasks get a turn first.
         */

        if (prev != sched_idle[cpu])
            rq_enqueue(cpu, prev);
        /* If prev IS the idle task we simply let it be superseded; it is
         * re-selected in step 5 if nothing else is runnable. */
        break;

    case SCHED_TASK_BLOCKED:
        /* Task voluntarily blocked (sched_block / sched_sleep_ms).
         * It is either in the sleep queue or waiting for an explicit
         * sched_unblock().  Do not re-enqueue. */
        break;

    case SCHED_TASK_DEAD:
        /* Task exited.  Stash for cleanup on the next schedule() call.
         * Only one dead task can be pending at a time on a core; if the
         * slot is already occupied something has gone seriously wrong. */
        if (sched_cleanup[cpu])
            PANIC("sched: double-dead task on same core");
        sched_cleanup[cpu] = prev;
        break;

    case SCHED_TASK_READY:
        /* Already in a ready queue or just unblocked. No action needed. */
        break;
    }

    /* ── Step 5: select next task ── */
    struct task* next = rq_dequeue(cpu, /*allow_pid0=*/1);

    if (!next)
    {
        /* Work-steal from other cores; never steal PID-0 tasks. */
        for (int i = 1; i <= SCHED_NUM_CORES; i++)
        {
            int victim = (cpu + i) % SCHED_NUM_CORES;
            next = rq_dequeue(victim, /*allow_pid0=*/0);
            if (next)
                break;
        }
    }

    if (!next)
        next = sched_idle[cpu];

    /* ── Step 6: context switch ── */
    next->state = SCHED_TASK_RUNNING;
    sched_core_pid[cpu] = (int)next->pid;

    /*
     * Publish the new task pointer and address space before the register
     * switch so that any exception arriving immediately after sees a
     * consistent state.
     */
    asm volatile("msr tpidr_el1, %0" ::"r"(next));
    asm volatile("msr ttbr0_el1, %0" ::"r"(next->ttbr0));
    asm volatile("dsb sy");
    asm volatile("isb");

    if (prev != next)
    {
        /*
         * Validate the incoming context before trusting the stack pointer.
         * A zero LR or SP almost certainly means an uninitialised or
         * corrupted task struct.
         */
        if (next->context.lr == 0 || next->context.sp == 0)
        {
            printf("SCHED: corrupt context — task id=%lu pid=%u "
                   "lr=0x%lx sp=0x%lx\n",
                   next->id,
                   (unsigned)next->pid,
                   next->context.lr,
                   next->context.sp);
            PANIC("sched: corrupt task context");
        }

        switch_context(&prev->context, &next->context);
        /*
         * We return here when 'prev' is next scheduled.  At that point
         * tpidr_el1 has already been restored to 'prev' by the switch
         * that resumed us, so sched_get_current() is valid again.
         */
    }

    irq_restore(flags);
}
