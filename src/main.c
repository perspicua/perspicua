#include "driver/uart.h"
#include "lib/stdio.h"
#include "kernel/timer.h"
#include "driver/gic.h"
#include "kernel/pmm.h"
#include "kernel/mmu.h"

int main()
{
    uart_init();
    uart_enable_interrupts();

    printf("\nBoot complete.\n");

    pmm_init();
    mmu_init();

    void* page1 = pmm_alloc_page();
    void* page2 = pmm_alloc_page();

    printf("Allocated Page 1 at: 0x%x\n", (unsigned long)page1);
    printf("Allocated Page 2 at: 0x%x\n", (unsigned long)page2);

    printf("Freeing Page 1...\n");
    pmm_free_page(page1);

    void* page3 = pmm_alloc_page();
    printf("Allocated Page 3 at: 0x%x\n", (unsigned long)page3);

    gic_init();
    timer_interrupt_init();
    enable_interrupts();
    return 0;
}
