#include "gic.h"
#include "../lib/stdio.h"
#include "../devicetree/pht.h"
#include "../lib/panic.h"
#include "../kernel/addr.h"
#include "uart.h"

volatile unsigned int* GICD_CTLR = NULL;
volatile unsigned int* GICD_ISENABLERn = NULL;
volatile unsigned char* GICD_IPRIORITYR = NULL;
volatile unsigned char* GICD_ITARGETSR = NULL;
volatile unsigned int* GICD_SGIR = NULL;

volatile unsigned int* GICC_CTLR = NULL;
volatile unsigned int* GICC_PMR = NULL;
volatile unsigned int* GICC_IAR = NULL;
volatile unsigned int* GICC_EOIR = NULL;

void gic_send_panic_ipi(void)
{
    // SGI ID 0, target filter 0b10 = all cores except self
    *GICD_SGIR = (0b10 << 24) | 0;
}

void gic_init(void)
{
    struct pht_node* gic_node = pht_find_device("gic");
    if (gic_node == NULL)
    {
        PANIC("[  GIC ] Device node not found in hardware tree!\n");
    }
    uintptr_t gicd_vbase = P2V(gic_node->address[0]);
    uintptr_t gicc_vbase = P2V(gic_node->address[1]);

    GICD_CTLR = (volatile unsigned int*)(gicd_vbase + 0x000);
    GICD_ISENABLERn = (volatile unsigned int*)(gicd_vbase + 0x100);
    GICD_IPRIORITYR = (volatile unsigned char*)(gicd_vbase + 0x400);
    GICD_ITARGETSR = (volatile unsigned char*)(gicd_vbase + 0x800);
    GICD_SGIR = (volatile unsigned int*)(gicd_vbase + 0xF00);

    GICC_CTLR = (volatile unsigned int*)(gicc_vbase + 0x000);
    GICC_PMR = (volatile unsigned int*)(gicc_vbase + 0x004);
    GICC_IAR = (volatile unsigned int*)(gicc_vbase + 0x00C);
    GICC_EOIR = (volatile unsigned int*)(gicc_vbase + 0x010);

    *GICD_CTLR = 1;

    GICD_ISENABLERn[TIMER_IRQ / 32] = (1 << (TIMER_IRQ % 32));
    GICD_IPRIORITYR[TIMER_IRQ] = 0;

    unsigned int uart_irq = uart_get_irq();
    GICD_ISENABLERn[uart_irq / 32] = (1 << (uart_irq % 32));
    GICD_IPRIORITYR[uart_irq] = 0;
    GICD_ITARGETSR[uart_irq] = 0x01;

    *GICC_CTLR = 1;
    *GICC_PMR = 0xFF;

    printf("[  GIC ] GIC-400 distributor @ 0x%lx, CPU interface @ 0x%lx\n", (unsigned long)0xFF841000,
           (unsigned long)0xFF842000);
    printf("[  GIC ] IRQ %d (phys timer) enabled, IRQ %d (UART0) → CPU0\n", TIMER_IRQ, uart_irq);
}

void gic_secondary_init(void)
{
    *GICC_CTLR = 1;
    *GICC_PMR = 0xFF;
    GICD_ISENABLERn[TIMER_IRQ / 32] = (1 << (TIMER_IRQ % 32));
}
