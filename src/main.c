#include "driver/uart.h"
#include "lib/stdio.h"
#include "kernel/timer.h"
#include "driver/gic.h"
#include "kernel/pmm.h"
#include "kernel/mmu.h"
#include "kernel/heap.h"
#include "kernel/sched.h"

void task_a(void)
{
    for (int i = 0; i < 50; i++)
    {
        printf("[Task A] count = %d\n", i);
        sleep_ms(500);
    }
}

void task_b(void)
{
    for (int i = 0; i < 50; i++)
    {
        printf("[Task B] count = %d\n", i);
        sleep_ms(500);
    }
}

int main()
{
    uart_init();
    uart_enable_interrupts();

    printf("\nBoot complete.\n");

    pmm_init();
    mmu_init();
    heap_init();

    gic_init();
    timer_interrupt_init();

    sched_init();
    sched_create_task(task_a);
    sched_create_task(task_b);

    enable_interrupts();

    // idle loop — scheduler preempts this via timer IRQ
    while (1)
        asm volatile("wfe");

    return 0;
}
