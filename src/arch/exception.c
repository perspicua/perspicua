#include "exception.h"
#include "../driver/uart.h"
#include "../driver/gic.h"
#include "../lib/stdio.h"
#include "../lib/panic.h"
#include "../kernel/timer.h"
#include "../kernel/sched.h"

void c_exception_handler()
{
    unsigned int esr;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));

    unsigned long elr;
    asm volatile("mrs %0, elr_el1" : "=r"(elr));

    unsigned long far;
    asm volatile("mrs %0, far_el1" : "=r"(far));

    printf("An unhandled exception occurred!\n");
    printf("FAR_EL1 (Faulting Address): 0x%lx\n", far);
    printf("ESR_EL1 (Reason)  : 0x%lx\n", (unsigned long)esr);
    printf("ELR_EL1 (Address) : 0x%lx\n", elr);
    printf("System halted.\n");

    while (1)
    {
        asm volatile("wfe");
    }
}
void c_irq_handler(void)
{
    // check before even reading IAR — a panic IPI may have woken us
    if (kernel_panicked)
    {
        disable_interrupts();
        for (;;)
            asm volatile("wfe");
    }

    unsigned int iar = GICC_IAR;
    unsigned int irq_id = iar & 0x3FF;

    if (irq_id >= 1020) // spurious interrupt — do NOT write EOIR
        return;

    if (irq_id == 0) // SGI 0: panic IPI from another core
    {
        GICC_EOIR = iar;
        disable_interrupts();
        for (;;)
            asm volatile("wfe");
    }
    else if (irq_id == TIMER_IRQ)
    {
        timer_interrupt_reset();
        GICC_EOIR = iar;
        schedule();
        return;
    }
    else if (irq_id == UART_IRQ)
    {
        while (uart_data_ready())
        {
            char c = uart_getc();
            if (c == '\n')
                uart_send('\r');
            uart_send(c);
        }
        uart_clear_interrupt();
    }

    GICC_EOIR = iar;
}
