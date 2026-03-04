#include "sched.h"
#include "heap.h"
#include "pmm.h"
#include "timer.h"
#include "lock.h"
#include "../lib/stdio.h"
#include "../lib/string.h"

static struct task* current_task_ptr[4] = {0, 0, 0, 0};
static struct task* idle_task_ptr[4] = {0, 0, 0, 0};

// shared queues protected by a spinlock
static struct task* ready_queue_head = 0;
static struct task* ready_queue_tail = 0;
static struct task* sleep_queue_head = 0;
static struct task* task_to_free[4] = {0, 0, 0, 0};
static int next_task_id = 0;

static spinlock_t sched_lock = SPINLOCK_INIT;

// --- queue helpers (must be called while holding sched_lock) ---
static void enqueue_ready(struct task* t)
{
    t->next = 0;
    if (!ready_queue_head)
    {
        ready_queue_head = t;
        ready_queue_tail = t;
    }
    else
    {
        ready_queue_tail->next = t;
        ready_queue_tail = t;
    }
}

static struct task* dequeue_ready(void)
{
    if (!ready_queue_head)
        return 0;
    struct task* t = ready_queue_head;
    ready_queue_head = ready_queue_head->next;
    if (!ready_queue_head)
        ready_queue_tail = 0;
    t->next = 0;
    return t;
}

static void insert_sleep(struct task* t)
{
    if (!sleep_queue_head || t->wake_time < sleep_queue_head->wake_time)
    {
        t->next = sleep_queue_head;
        sleep_queue_head = t;
        return;
    }
    struct task* curr = sleep_queue_head;
    while (curr->next && curr->next->wake_time <= t->wake_time)
    {
        curr = curr->next;
    }
    t->next = curr->next;
    curr->next = t;
}

// --- task execution ---
static void task_wrapper(void (*entry)(void))
{
    enable_interrupts();
    entry();

    disable_interrupts();
    unsigned long core_id;
    asm volatile("mrs %0, mpidr_el1" : "=r"(core_id));
    core_id &= 3;

    current_task_ptr[core_id]->state = TASK_DEAD;
    schedule();

    while (1)
        asm volatile("wfe");
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
    unsigned char* stack = (unsigned char*)pmm_alloc_pages(2);
    unsigned long sp = ((unsigned long)stack + TASK_STACK_SIZE) & ~15UL;

    idle->state = TASK_READY;
    idle->id = 900 + id; // idle tasks get ids 900, 901, 902, 903
    idle->stack = stack;
    idle->context.sp = sp;
    idle->context.lr = (unsigned long)task_wrapper;
    idle->context.x19 = (unsigned long)idle_task_entry;
    return idle;
}

// --- core scheduler api ---
void sched_init(void)
{ // called by core 0
    struct task* main_task = (struct task*)kmalloc(sizeof(struct task));
    main_task->state = TASK_RUNNING;
    main_task->id = next_task_id++;
    main_task->stack = 0;
    main_task->next = 0;

    current_task_ptr[0] = main_task;
    idle_task_ptr[0] = create_idle_task(0);

    printf("SCHED: SMP O(1) Queues Initialized.\n");
}

void sched_secondary_init(void)
{ // called by cores 1, 2, 3
    unsigned long core_id;
    asm volatile("mrs %0, mpidr_el1" : "=r"(core_id));
    core_id &= 3;

    idle_task_ptr[core_id] = create_idle_task(core_id);

    // forcefully load the idle task context to start the scheduling loop
    current_task_ptr[core_id] = idle_task_ptr[core_id];
    current_task_ptr[core_id]->state = TASK_RUNNING;

    enable_interrupts();
    schedule();
    while (1)
        asm volatile("wfe"); // fallback
}

void sched_create_task(void (*entry)(void))
{
    struct task* t = (struct task*)kmalloc(sizeof(struct task));
    memset(t, 0, sizeof(struct task));
    unsigned char* stack = (unsigned char*)pmm_alloc_pages(2);
    unsigned long sp = ((unsigned long)stack + TASK_STACK_SIZE) & ~15UL;

    t->state = TASK_READY;
    t->context.sp = sp;
    t->context.lr = (unsigned long)task_wrapper;
    t->context.x19 = (unsigned long)entry;

    unsigned long flags = spin_lock_irqsave(&sched_lock);
    t->id = next_task_id++;
    t->stack = stack;
    enqueue_ready(t);
    spin_unlock_irqrestore(&sched_lock, flags);
}

void sched_sleep_ms(unsigned long ms)
{
    unsigned long core_id;
    asm volatile("mrs %0, mpidr_el1" : "=r"(core_id));
    core_id &= 3;

    struct task* curr = current_task_ptr[core_id];
    if (!curr || curr == idle_task_ptr[core_id])
        return;

    unsigned long flags = spin_lock_irqsave(&sched_lock);
    curr->state = TASK_BLOCKED;
    curr->wake_time = get_system_time() + ms;
    insert_sleep(curr);
    spin_unlock_irqrestore(&sched_lock, flags);

    schedule();
}

void schedule(void)
{
    unsigned long core_id;
    asm volatile("mrs %0, mpidr_el1" : "=r"(core_id));
    core_id &= 3;

    // free dead task from prior schedule() outside the lock
    if (task_to_free[core_id])
    {
        if (task_to_free[core_id]->stack)
            pmm_free_pages(task_to_free[core_id]->stack, 2);
        kfree(task_to_free[core_id]);
        task_to_free[core_id] = 0;
    }

    unsigned long flags = spin_lock_irqsave(&sched_lock); // LOCK THE QUEUE!
    unsigned long now = get_system_time();

    while (sleep_queue_head && sleep_queue_head->wake_time <= now)
    {
        struct task* w = sleep_queue_head;
        sleep_queue_head = w->next;
        w->state = TASK_READY;
        enqueue_ready(w);
    }

    struct task* prev = current_task_ptr[core_id];

    if (prev->state == TASK_RUNNING)
    {
        if (prev != idle_task_ptr[core_id])
        {
            prev->state = TASK_READY;
            enqueue_ready(prev);
        }
    }
    else if (prev->state == TASK_DEAD)
    {
        task_to_free[core_id] = prev;
    }

    struct task* next = dequeue_ready();

    if (!next)
    {
        next = idle_task_ptr[core_id];
    }

    next->state = TASK_RUNNING;
    current_task_ptr[core_id] = next;

    spin_unlock_irqrestore(&sched_lock, flags); // UNLOCK BEFORE SWITCHING!

    if (prev != next)
    {
        switch_context(&prev->context, &next->context);
    }
}
