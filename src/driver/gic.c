#include "gic.h"
void gic_init(void)
{
    GICD_CTLR = 1;

    GICD_ISENABLERn(TIMER_IRQ / 32) = (1 << (TIMER_IRQ % 32));
    GICD_IPRIORITYR[TIMER_IRQ] = 0;

    GICD_ISENABLERn(UART_IRQ / 32) = (1 << (UART_IRQ % 32));
    GICD_IPRIORITYR[UART_IRQ] = 0;
    GICD_ITARGETSR[UART_IRQ] = 0x01;

    GICC_CTLR = 1;
    GICC_PMR = 0xFF;
}

void gic_secondary_init(void)
{
    GICC_CTLR = 1;
    GICC_PMR = 0xFF;
}
