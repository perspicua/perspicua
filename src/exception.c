#include "exception.h"
#include "mmio/uart.h"
#include "mmio/gic.h"
#include "stdio.h"
#include "timer.h"

void c_exception_handler()
{
    unsigned int esr;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));

    unsigned long elr;
    asm volatile("mrs %0, elr_el1" : "=r"(elr));

    printf("An unhandled exception occurred!\n");
    printf("ESR_EL1 (Reason)  : 0x%x\n", esr);
    printf("ELR_EL1 (Address) : 0x%x\n", elr);
    printf("System halted.\n");

    while (1)
    {
        asm volatile("wfe");
    }
}
void c_irq_handler(void)
{
    unsigned int iar = GICC_IAR;
    unsigned int irq_id = iar & 0x3FF;

    if (irq_id == TIMER_IRQ)
    {
        printf("\n[Background IRQ]: Tick! 1 second has passed.\n");
        timer_interrupt_reset();
    }
    else if (irq_id == UART_IRQ)
    {
        while (uart_data_ready())
        {
            char c = uart_getc();
            uart_send(c);
            if (c == '\n')
                uart_send('\r');
        }
        uart_clear_interrupt();
    }

    GICC_EOIR = iar;
}
