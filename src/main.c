#include "driver/uart.h"
#include "driver/gic.h"

#include "kernel/pmm.h"
#include "kernel/mmu.h"
#include "kernel/heap.h"
#include "kernel/sched.h"
#include "kernel/timer.h"

#include "lib/stdio.h"
#include "lib/string.h"

void task_cpu_hog(void)
{
    printf("[HOG] Starting heavy calculation...\n");
    volatile unsigned long counter = 0;
    while (counter < 500000000)
    {
        counter++;
    }
    printf("[HOG] Finished!\n");
}

void task_ticker(void)
{
    for (int i = 0; i < 5; i++)
    {
        printf("[TICKER] Tick %d! The hog hasn't frozen the system.\n", i);
        sched_sleep_ms(100);
    }
}
void task_sleep_short(void)
{
    sched_sleep_ms(100);
    printf("[SLEEP] Task Short woke up! (Expected 1st)\n");
}

void task_sleep_long(void)
{
    sched_sleep_ms(500);
    printf("[SLEEP] Task Long woke up! (Expected 3rd)\n");
}

void task_sleep_med(void)
{
    sched_sleep_ms(300);
    printf("[SLEEP] Task Medium woke up! (Expected 2nd)\n");
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
    printf("\nBoot complete.\n");

    pmm_init();
    mmu_init();
    heap_init();
    gic_init();
    timer_interrupt_init();
    sched_init();

    sched_create_task(task_sleep_long);
    sched_create_task(task_sleep_short);
    sched_create_task(task_sleep_med);

    sched_create_task(task_cpu_hog);
    sched_create_task(task_ticker);

    sched_create_task(spawn_storm);

    enable_interrupts();

    while (1)
        asm volatile("wfe");

    return 0;
}
