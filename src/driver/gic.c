#include "gic.h"
#include "../lib/stdio.h"

void gic_send_panic_ipi(void)
{
    // SGI ID 0, target filter 0b10 = all cores except self
    GICD_SGIR = (0b10 << 24) | 0;
}

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

    printf("[  GIC ] GIC-400 distributor @ 0x%lx, CPU interface @ 0x%lx\n", (unsigned long)0xFF841000,
           (unsigned long)0xFF842000);
    printf("[  GIC ] IRQ %d (phys timer) enabled, IRQ %d (UART0) → CPU0\n", TIMER_IRQ, UART_IRQ);
}

void gic_secondary_init(void)
{
    GICC_CTLR = 1;
    GICC_PMR = 0xFF;
    GICD_ISENABLERn(TIMER_IRQ / 32) = (1 << (TIMER_IRQ % 32));
}
