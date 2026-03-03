#include "sched.h"
#include "heap.h"
#include "pmm.h"
#include "timer.h"
#include "../lib/stdio.h"

static struct task* tasks[MAX_TASKS];
static int num_tasks;
static int current_task;

// wrapper that kills the task when its entry function returns
static void task_wrapper(void (*entry)(void))
{
    // we arrived here via switch_context from an IRQ handler,
    // so IRQs are still masked. re-enable them.
    enable_interrupts();

    entry();

    // task returned, mark it dead
    tasks[current_task]->state = TASK_DEAD;
    printf("SCHED: Task %d exited.\n", (int)tasks[current_task]->id);

    // yield to next task, never returns
    schedule();

    // should never reach here
    while (1)
        asm volatile("wfe");
}

void sched_init(void)
{
    // task 0 is the current (main) kernel thread
    struct task* main_task = (struct task*)kmalloc(sizeof(struct task));
    main_task->state = TASK_RUNNING;
    main_task->id = 0;
    main_task->stack = 0; // uses the boot stack

    tasks[0] = main_task;
    num_tasks = 1;
    current_task = 0;

    printf("SCHED: Initialized.\n");
}

void sched_create_task(void (*entry)(void))
{
    if (num_tasks >= MAX_TASKS)
    {
        printf("SCHED: Max tasks reached.\n");
        return;
    }

    struct task* t = (struct task*)kmalloc(sizeof(struct task));
    // allocate stack directly from PMM (full page, page-aligned)
    unsigned char* stack = (unsigned char*)pmm_alloc_page();

    // stack grows downward, start at the top, 16-byte aligned
    unsigned long sp = ((unsigned long)stack + TASK_STACK_SIZE) & ~15UL;

    t->state = TASK_READY;
    t->id = num_tasks;
    t->stack = stack;

    // set up context so switch_context lands in task_wrapper
    t->context.sp = sp;
    t->context.lr = (unsigned long)task_wrapper;
    t->context.x19 = (unsigned long)entry; // first arg to task_wrapper
    t->context.fp = 0;

    // zero remaining callee-saved regs
    t->context.x20 = 0;
    t->context.x21 = 0;
    t->context.x22 = 0;
    t->context.x23 = 0;
    t->context.x24 = 0;
    t->context.x25 = 0;
    t->context.x26 = 0;
    t->context.x27 = 0;
    t->context.x28 = 0;

    tasks[num_tasks] = t;
    num_tasks++;

    printf("SCHED: Created task %d.\n", (int)t->id);
}

void sched_sleep_ms(unsigned long ms)
{
    if (current_task == 0)
        return;
    unsigned long flags = irq_save();
    tasks[current_task]->state = TASK_BLOCKED;
    tasks[current_task]->wake_time = get_system_time() + ms;
    irq_restore(flags);
    schedule();
}

void schedule(void)
{
    unsigned long now = get_system_time();
    for (int i = 0; i <= num_tasks; i++)
    {
        if (tasks[i]->state == TASK_BLOCKED && tasks[i]->wake_time >= now)
            tasks[i]->state = TASK_READY;
    
    }

    for (int i = num_tasks - 1; i >= 0; i--)
    {
        if (tasks[i]->state == TASK_DEAD && i != current_task)
        {
            if (tasks[i]->stack)
                pmm_free_page(tasks[i]->stack);
            kfree(tasks[i]);

            // compact the array
            for (int j = i; j < num_tasks - 1; j++)
                tasks[j] = tasks[j + 1];
            tasks[num_tasks - 1] = 0;
            num_tasks--;

            if (current_task > i)
                current_task--;
        }
    }

    
    if (num_tasks <= 1)
        return;

    int prev = current_task;
    int next = current_task;

    // round-robin: find the next ready/running task
    for (int i = 1; i <= num_tasks; i++)
    {
        int idx = (current_task + i) % num_tasks;
        if (tasks[idx]->state == TASK_READY || tasks[idx]->state == TASK_RUNNING)
        {
            next = idx;
            break;
        }
    }

    if (next == prev)
        return;

    // update states
    if (tasks[prev]->state == TASK_RUNNING)
        tasks[prev]->state = TASK_READY;
    tasks[next]->state = TASK_RUNNING;
    current_task = next;

    switch_context(&tasks[prev]->context, &tasks[next]->context);
}
