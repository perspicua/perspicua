#include "driver/uart.h"
#include "driver/gic.h"

#include "kernel/pmm.h"
#include "kernel/mmu.h"
#include "kernel/heap.h"
#include "kernel/sched.h"
#include "kernel/timer.h"
#include "kernel/lock.h"

#include "lib/panic.h"
#include "lib/stdio.h"
#include "lib/string.h"

#define KERNEL_VMA 0xFFFFFF8000000000ULL
#define V2P(v) ((unsigned long)(v) - KERNEL_VMA)
#define P2V(p) ((unsigned long)(p) + KERNEL_VMA)

extern void _entry(void);

static spinlock_t console_lock = SPINLOCK_INIT;

static inline unsigned long get_core_id(void)
{
    unsigned long core_id;
    asm volatile("mrs %0, mpidr_el1" : "=r"(core_id));
    return core_id & 3;
}

void smp_init(void)
{
    printf("\nSMP: Waking up secondary cores...\n");

    unsigned long entry_phys = V2P((unsigned long)_entry);

    volatile unsigned long* spin_cpu1 = (unsigned long*)P2V(0xE0);
    volatile unsigned long* spin_cpu2 = (unsigned long*)P2V(0xE8);
    volatile unsigned long* spin_cpu3 = (unsigned long*)P2V(0xF0);

    *spin_cpu1 = entry_phys;
    *spin_cpu2 = entry_phys;
    *spin_cpu3 = entry_phys;

    // wake up parked cores
    asm volatile("sev");
}

void secondary_main(void)
{
    unsigned long core_id;
    asm volatile("mrs %0, mpidr_el1" : "=r"(core_id));
    core_id &= 3;

    mmu_secondary_init();

    unsigned long flags = spin_lock_irqsave(&console_lock);
    printf("SMP: Core %lu is awake, MMU enabled!\n", core_id);
    spin_unlock_irqrestore(&console_lock, flags);

    gic_secondary_init();
    timer_interrupt_init();

    sched_secondary_init();
}

static volatile int shared_counter = 0;
static spinlock_t counter_lock = SPINLOCK_INIT;

void task_counter_increment(void)
{
    unsigned long core = get_core_id();

    for (int i = 0; i < 1000000; i++)
    {
        unsigned long flags = spin_lock_irqsave(&counter_lock);

        shared_counter++;

        spin_unlock_irqrestore(&counter_lock, flags);
    }

    unsigned long flags = spin_lock_irqsave(&console_lock);
    printf("[TEST 1] Core %lu finished counting! Current total: %d\n", core, shared_counter);
    spin_unlock_irqrestore(&console_lock, flags);
}

void run_spinlock_test(void)
{
    unsigned long flags = spin_lock_irqsave(&console_lock);
    printf("\n--- Starting Spinlock Stress Test ---\n");
    spin_unlock_irqrestore(&console_lock, flags);
    shared_counter = 0;

    // Spawn 4 tasks. If the scheduler is fair, one should land on each core.
    sched_create_task(task_counter_increment);
    sched_create_task(task_counter_increment);
    sched_create_task(task_counter_increment);
    sched_create_task(task_counter_increment);
    // final task should print 4000000
}

void task_heavy_cpu(void)
{
    unsigned long core = get_core_id();
    int iteration = 0;

    while (1)
    {
        volatile int dummy = 0;
        for (int i = 0; i < 50000000; i++)
        {
            dummy += i;
        }

        unsigned long flags = spin_lock_irqsave(&console_lock);
        printf("[TEST 2] Core %lu is still crunching numbers... (Iteration %d)\n", core, iteration++);
        spin_unlock_irqrestore(&console_lock, flags);
    }
}

void run_cpu_load_test(void)
{
    printf("\n--- Starting Heavy CPU Load Test ---\n");
    for (int i = 0; i < 8; i++)
    {
        // constant context switch
        sched_create_task(task_heavy_cpu);
    }
}
int main()
{
    uart_init();
    printf("Hello from the Higher Half! main() is at: 0x%lx\n", (unsigned long)main);
    uart_enable_interrupts();

    pmm_init();
    mmu_init();
    heap_init();
    gic_init();
    timer_interrupt_init();
    sched_init();

    smp_init();
    printf("Boot complete\n");

    run_spinlock_test();
    run_cpu_load_test();
    sleep_ms(1000);
    panic("Test panic", __FILE__, __LINE__);
    enable_interrupts();

    while (1)
        asm volatile("wfe");

    return 0;
}
