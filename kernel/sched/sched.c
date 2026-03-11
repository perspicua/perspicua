#include "sched.h"
#include "heap.h"
#include "pmm.h"
#include "mmu.h"
#include "addr.h"
#include "timer.h"
#include "lock.h"
#include "process.h"
#include "stdio.h"
#include "string.h"
#include "panic.h"

// Static assertions to ensure assembly offsets remain correct
_Static_assert(sizeof(struct cpu_context) == 104, "struct cpu_context size mismatch");
_Static_assert(__builtin_offsetof(struct task, state) == 112, "struct task->state offset mismatch");

#define STACK_CANARY 0xDEADC0DEDEADC0DEULL
#define STACK_PAGES 3 // 1 guard page + 2 usable pages (8 KB usable)

static struct task* idle_task_ptr[4] = {0, 0, 0, 0};
static struct task init_tasks[4];

// per-CPU ready queues
static struct task* ready_queue_head[4] = {0, 0, 0, 0};
static struct task* ready_queue_tail[4] = {0, 0, 0, 0};
static spinlock_t ready_queue_lock[4] = {SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT};

static struct task* sleep_queue_head = 0;
static spinlock_t sleep_queue_lock = SPINLOCK_INIT;

static struct task* task_to_free[4] = {0, 0, 0, 0};
static struct task* last_switched_from[4] = {0, 0, 0, 0};

static int next_task_id = 0;
static spinlock_t next_task_id_lock = SPINLOCK_INIT;

static inline int get_core_id(void)
{
    unsigned long core_id;
    asm volatile("mrs %0, mpidr_el1" : "=r"(core_id));
    return (int)(core_id & 3);
}

// --- queue helpers ---
static void enqueue_ready(int cpu, struct task* t)
{
    unsigned long flags = spin_lock_irqsave(&ready_queue_lock[cpu]);
    t->next = 0;
    if (!ready_queue_head[cpu])
    {
        ready_queue_head[cpu] = t;
        ready_queue_tail[cpu] = t;
    }
    else
    {
        ready_queue_tail[cpu]->next = t;
        ready_queue_tail[cpu] = t;
    }
    spin_unlock_irqrestore(&ready_queue_lock[cpu], flags);
}

static struct task* dequeue_ready(int cpu)
{
    unsigned long flags = spin_lock_irqsave(&ready_queue_lock[cpu]);
    if (!ready_queue_head[cpu])
    {
        spin_unlock_irqrestore(&ready_queue_lock[cpu], flags);
        return 0;
    }
    struct task* t = ready_queue_head[cpu];
    ready_queue_head[cpu] = ready_queue_head[cpu]->next;
    if (!ready_queue_head[cpu])
        ready_queue_tail[cpu] = 0;
    t->next = 0;
    spin_unlock_irqrestore(&ready_queue_lock[cpu], flags);
    return t;
}

static void insert_sleep(struct task* t)
{
    unsigned long flags = spin_lock_irqsave(&sleep_queue_lock);
    if (!sleep_queue_head || t->wake_time < sleep_queue_head->wake_time)
    {
        t->next = sleep_queue_head;
        sleep_queue_head = t;
        spin_unlock_irqrestore(&sleep_queue_lock, flags);
        return;
    }
    struct task* curr = sleep_queue_head;
    while (curr->next && curr->next->wake_time <= t->wake_time)
    {
        curr = curr->next;
    }
    t->next = curr->next;
    curr->next = t;
    spin_unlock_irqrestore(&sleep_queue_lock, flags);
}

// --- task execution ---

extern void task_wrapper_asm(void);

void post_switch_hook(void)
{
    int cpu = get_core_id();
    struct task* sw_from = last_switched_from[cpu];
    if (sw_from)
    {
        last_switched_from[cpu] = NULL;
        if (sw_from->state == TASK_READY)
        {
            enqueue_ready(cpu, sw_from);
        }
    }
}

static void idle_task_entry(void)
{
    while (1)
        asm volatile("wfe");
}

static struct task* create_idle_task(int id)
{
    struct task* idle = (struct task*)kmalloc(sizeof(struct task));
    memset(idle, 0, sizeof(struct task));
    unsigned char* stack = (unsigned char*)pmm_alloc_pages(STACK_PAGES);
    mmu_unmap_page((unsigned long)stack);
    unsigned char* usable = stack + PAGE_SIZE;
    unsigned long sp = ((unsigned long)usable + TASK_STACK_SIZE) & ~15UL;
    *(unsigned long*)usable = STACK_CANARY;

    idle->state = TASK_READY;
    idle->id = 900 + id;
    idle->pid = 0;
    idle->stack = stack;
    idle->ttbr0 = mmu_kernel_ttbr0();
    idle->context.sp = sp;
    idle->context.lr = (unsigned long)task_wrapper_asm;
    idle->context.x19 = (unsigned long)idle_task_entry;
    return idle;
}

// --- core scheduler api ---
void sched_init(void)
{
    struct task* main_task = &init_tasks[0];
    memset(main_task, 0, sizeof(struct task));
    main_task->state = TASK_RUNNING;
    main_task->id = next_task_id++;
    main_task->pid = 0;
    main_task->stack = 0;
    main_task->ttbr0 = mmu_kernel_ttbr0();

    asm volatile("msr tpidr_el1, %0" : : "r"(main_task));
    idle_task_ptr[0] = create_idle_task(0);

    printf("[SCHED ] Task 0 (main) bound to CPU0, idle task spawned\n");
}

void sched_secondary_init(void)
{
    int core_id = get_core_id();

    struct task* boot_task = &init_tasks[core_id];
    memset(boot_task, 0, sizeof(struct task));
    boot_task->state = TASK_DEAD;
    boot_task->id = 800 + core_id;
    boot_task->pid = 0;
    asm volatile("msr tpidr_el1, %0" : : "r"(boot_task));

    idle_task_ptr[core_id] = create_idle_task(core_id);

    schedule();
}

void sched_create_task(void (*entry)(void))
{
    struct task* t = (struct task*)kmalloc(sizeof(struct task));
    memset(t, 0, sizeof(struct task));
    unsigned char* stack = (unsigned char*)pmm_alloc_pages(STACK_PAGES);
    mmu_unmap_page((unsigned long)stack);
    unsigned char* usable = stack + PAGE_SIZE;
    unsigned long sp = ((unsigned long)usable + TASK_STACK_SIZE) & ~15UL;
    *(unsigned long*)usable = STACK_CANARY;

    t->state = TASK_READY;
    t->context.sp = sp;
    t->context.lr = (unsigned long)task_wrapper_asm;
    t->context.x19 = (unsigned long)entry;
    t->ttbr0 = mmu_kernel_ttbr0();
    t->pid = 0;

    unsigned long flags = spin_lock_irqsave(&next_task_id_lock);
    t->id = next_task_id++;
    spin_unlock_irqrestore(&next_task_id_lock, flags);

    t->stack = stack;
    enqueue_ready(get_core_id(), t);
}

void sched_sleep_ms(unsigned long ms)
{
    unsigned long flags = irq_save();
    struct task* curr = sched_get_current();
    int cpu = get_core_id();
    if (!curr || curr == idle_task_ptr[cpu])
    {
        irq_restore(flags);
        return;
    }

    curr->state = TASK_BLOCKED;
    curr->wake_time = get_system_time() + ms;
    insert_sleep(curr);

    schedule();
    irq_restore(flags);
}

void sched_create_user_task(unsigned long forged_sp, unsigned long forged_lr, uint32_t pid)
{
    struct task* t = (struct task*)kmalloc(sizeof(struct task));
    memset(t, 0, sizeof(struct task));

    t->state = TASK_READY;
    t->context.sp = forged_sp;
    t->context.lr = forged_lr;

    t->stack = (unsigned char*)process_table[pid].vaddr_kernel_stack;

    t->ttbr0 = process_get_ttbr0(pid);
    t->pid = pid;

    unsigned long flags = spin_lock_irqsave(&next_task_id_lock);
    t->id = next_task_id++;
    spin_unlock_irqrestore(&next_task_id_lock, flags);

    enqueue_ready(get_core_id(), t);
}

void sched_block(void)
{
    unsigned long flags = irq_save();
    struct task* curr = sched_get_current();
    if (!curr || curr == idle_task_ptr[get_core_id()])
    {
        irq_restore(flags);
        return;
    }

    curr->state = TASK_BLOCKED;
    schedule();
    irq_restore(flags);
}

void sched_unblock(struct task* t)
{
    unsigned long flags = irq_save();
    if (t->state == TASK_BLOCKED)
    {
        t->state = TASK_READY;
        enqueue_ready(get_core_id(), t);
    }
    irq_restore(flags);
}

struct task* sched_get_current(void)
{
    struct task* t;
    asm volatile("mrs %0, tpidr_el1" : "=r"(t));
    return t;
}

void schedule(void)
{
    unsigned long flags = irq_save();
    int cpu = get_core_id();

    // 1. cleanup tasks that died on this core
    struct task* dead = task_to_free[cpu];
    if (dead)
    {
        task_to_free[cpu] = 0;

        if (dead->pid != 0)
        {
            process_exit(dead->pid);
        }

        if (dead->stack)
        {
            unsigned long guard_va = (unsigned long)dead->stack;
            unsigned long guard_pa = V2P(guard_va);
            mmu_map_page(guard_va, guard_pa, MMU_FLAGS_KERNEL_RW);
            pmm_free_pages(dead->stack, STACK_PAGES);
        }
        if (dead < &init_tasks[0] || dead > &init_tasks[3])
            kfree(dead);
    }

    // 2. wake up sleeping tasks
    unsigned long now = get_system_time();
    unsigned long s_flags = spin_lock_irqsave(&sleep_queue_lock);
    while (sleep_queue_head && sleep_queue_head->wake_time <= now)
    {
        struct task* w = sleep_queue_head;
        sleep_queue_head = w->next;
        w->state = TASK_READY;
        enqueue_ready(cpu, w);
    }
    spin_unlock_irqrestore(&sleep_queue_lock, s_flags);

    struct task* prev = sched_get_current();
    if (!prev)
    {
        irq_restore(flags);
        return;
    }

    // 3. verify stack integrity
    if (prev->stack)
    {
        unsigned char* usable = prev->stack + PAGE_SIZE;
        if (*(unsigned long*)usable != STACK_CANARY)
            PANIC("Stack overflow: canary corrupted!");
    }

    // 4. handle current task state
    if (prev->state == TASK_RUNNING)
    {
        if (prev != idle_task_ptr[cpu])
        {
            prev->state = TASK_READY;
        }
    }
    else if (prev->state == TASK_DEAD)
    {
        task_to_free[cpu] = prev;
    }

    // 5. pick next task
    struct task* next = dequeue_ready(cpu);
    if (!next)
    {
        for (int i = 0; i < 4; i++)
        {
            if (i == cpu)
                continue;
            next = dequeue_ready(i);
            if (next)
                break;
        }
    }

    if (!next)
        next = idle_task_ptr[cpu];

    // 6. perform switch
    next->state = TASK_RUNNING;
    asm volatile("msr tpidr_el1, %0" : : "r"(next));

    if (prev != next)
    {
        last_switched_from[cpu] = prev;
        asm volatile("msr ttbr0_el1, %0\n"
                     "isb"
                     :
                     : "r"(next->ttbr0));
        switch_context(&prev->context, &next->context);

        post_switch_hook();
    }

    irq_restore(flags);
}
