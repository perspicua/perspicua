#include "mmio/uart.h"
#include "stdio.h"
#include "timer.h"
#include "mmio/gic.h"

int main()
{
    uart_init();
    uart_enable_interrupts();

    printf("\nBoot complete. Initializing background timer & interrupts...\n");

    gic_init();
    timer_interrupt_init();
    enable_interrupts();

    printf("Type anything!\n\n");

    while (1)
    {
        asm volatile("wfe");
    }
    return 0;
}
