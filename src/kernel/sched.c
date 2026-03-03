#include "sched.h"
#include "heap.h"
#include "pmm.h"
#include "timer.h"
#include "../lib/stdio.h"

static struct task* current_task_ptr = NULL;
static struct task* ready_queue_head = NULL;
static struct task* ready_queue_tail = NULL;
static struct task* sleep_queue_head = NULL;

static struct task* idle_task_ptr = NULL;
static struct task* task_to_free = NULL;
static int next_task_id = 0;

static void enqueue_ready(struct task* t)
{
    t->next = NULL;
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
        return NULL;
    struct task* t = ready_queue_head;
    ready_queue_head = ready_queue_head->next;
    if (!ready_queue_head)
        ready_queue_tail = NULL;
    t->next = NULL;
    return t;
}

static void insert_sleep(struct task* t)
{
    // sorted by wake_time (earliest wake time at the head)
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

static void task_wrapper(void (*entry)(void))
{
    enable_interrupts();
    entry();

    disable_interrupts();
    current_task_ptr->state = TASK_DEAD;
    printf("SCHED: Task %d exited.\n", (int)current_task_ptr->id);
    schedule();

    while (1)
        asm volatile("wfe");
}

static void idle_task_entry(void)
{
    while (1)
        asm volatile("wfe");
}

void sched_init(void)
{
    struct task* main_task = (struct task*)kmalloc(sizeof(struct task));
    main_task->state = TASK_RUNNING;
    main_task->id = next_task_id++;
    main_task->stack = 0;
    main_task->next = 0;

    current_task_ptr = main_task;

    idle_task_ptr = (struct task*)kmalloc(sizeof(struct task));
    unsigned char* stack = (unsigned char*)pmm_alloc_pages(2);
    unsigned long sp = ((unsigned long)stack + TASK_STACK_SIZE) & ~15UL;

    idle_task_ptr->state = TASK_READY;
    idle_task_ptr->id = 999;
    idle_task_ptr->stack = stack;
    idle_task_ptr->context.sp = sp;
    idle_task_ptr->context.lr = (unsigned long)task_wrapper;
    idle_task_ptr->context.x19 = (unsigned long)idle_task_entry;

    printf("SCHED: Initialized with O(1) Queues.\n");
}

void sched_create_task(void (*entry)(void))
{
    struct task* t = (struct task*)kmalloc(sizeof(struct task));
    unsigned char* stack = (unsigned char*)pmm_alloc_pages(2);
    unsigned long sp = ((unsigned long)stack + TASK_STACK_SIZE) & ~15UL;

    t->state = TASK_READY;
    t->id = next_task_id++;
    t->stack = stack;
    t->context.sp = sp;
    t->context.lr = (unsigned long)task_wrapper;
    t->context.x19 = (unsigned long)entry;

    unsigned long flags = irq_save();
    enqueue_ready(t);
    irq_restore(flags);

    printf("SCHED: Created task %d.\n", (int)t->id);
}

void sched_sleep_ms(unsigned long ms)
{
    if (!current_task_ptr || current_task_ptr == idle_task_ptr)
        return;

    unsigned long flags = irq_save();
    current_task_ptr->state = TASK_BLOCKED;
    current_task_ptr->wake_time = get_system_time() + ms;
    insert_sleep(current_task_ptr);
    irq_restore(flags);

    schedule();
}

void schedule(void)
{
    unsigned long flags = irq_save();
    unsigned long now = get_system_time();

    // free dead task from prev. context swithc
    if (task_to_free)
    {
        if (task_to_free->stack)
            pmm_free_page(task_to_free->stack);
        kfree(task_to_free);
        task_to_free = NULL;
    }

    // wake up sleeping tasks
    while (sleep_queue_head && sleep_queue_head->wake_time <= now)
    {
        struct task* w = sleep_queue_head;
        sleep_queue_head = w->next;
        w->state = TASK_READY;
        enqueue_ready(w);
    }

    struct task* prev = current_task_ptr;

    // handle currently running tasks
    if (prev->state == TASK_RUNNING)
    {
        if (prev == idle_task_ptr)
        {
            prev->state = TASK_READY;
        }
        else
        {
            prev->state = TASK_READY;
            enqueue_ready(prev);
        }
    }
    else if (prev->state == TASK_DEAD)
    {
        // cannot free stack while executing
        task_to_free = prev;
    }

    // get the next task
    struct task* next = dequeue_ready();

    // fallback to the idle task if nothing else is ready to run
    if (!next)
    {
        next = idle_task_ptr;
    }

    next->state = TASK_RUNNING;
    current_task_ptr = next;

    if (prev != next)
    {
        switch_context(&prev->context, &next->context);
    }

    irq_restore(flags);
}
