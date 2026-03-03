#include "driver/uart.h"
#include "driver/gic.h"

#include "kernel/pmm.h"
#include "kernel/mmu.h"
#include "kernel/heap.h"
#include "kernel/sched.h"
#include "kernel/timer.h"
#include "kernel/lock.h"

#include "lib/stdio.h"
#include "lib/string.h"

#define KERNEL_VMA 0xFFFFFF8000000000ULL
#define V2P(v) ((unsigned long)(v) - KERNEL_VMA)
#define P2V(p) ((unsigned long)(p) + KERNEL_VMA)

extern void _entry(void);

static spinlock_t console_lock = SPINLOCK_INIT;

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

    unsigned long flags = spin_lock_irqsave(&console_lock);
    printf("SMP: Core %lu is awake, MMU enabled!\n", core_id);
    spin_unlock_irqrestore(&console_lock, flags);

    gic_secondary_init();
    timer_interrupt_init();

    sched_secondary_init();
}

void task_short_lived(void)
{
    sched_sleep_ms(50);
}

void spawn_storm(void)
{
    printf("[STORM] Spawning 30 short-lived tasks...\n");
    for (int i = 0; i < 30; i++)
    {
        sched_create_task(task_short_lived);
    }
    printf("[STORM] All 30 tasks created successfully!\n");
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

    sched_create_task(spawn_storm);
    enable_interrupts();

    while (1)
        asm volatile("wfe");

    return 0;
}
