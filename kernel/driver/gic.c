/*
 * gic.c - Implementation of the GICv2 driver.
 *
 * This file handles discovery of the interrupt controller from the
 * hardware tree, register initialization, and IPI management.
 */

#include "driver/gic.h"

#include "uapi/syscalls.h"

#include "io.h"
#include "stdio.h"
#include "panic.h"
#include "addr.h"

#include "devicetree/pht.h"

#include "driver/uart.h"

/* Distributor registers */
volatile unsigned int* gic_d_ctlr = (void*)0;
volatile unsigned int* gic_d_isenablern = (void*)0;
volatile unsigned char* gic_d_ipriorityr = (void*)0;
volatile unsigned char* gic_d_itargetsr = (void*)0;
volatile unsigned int* gic_d_sgir = (void*)0;

/* CPU Interface registers */
volatile unsigned int* gic_c_ctlr = (void*)0;
volatile unsigned int* gic_c_pmr = (void*)0;
volatile unsigned int* gic_c_iar = (void*)0;
volatile unsigned int* gic_c_eoir = (void*)0;

static unsigned int cached_uart_irq = 0;

/*
 * gic_send_panic_ipi - Broadcasts a panic SGI to other cores.
 */
void gic_send_panic_ipi(void)
{
    // SGI ID 0, target filter 0b10 = all cores except self
    mmio_write(gic_d_sgir, (0b10 << 24) | 0);
}

/*
 * gic_init - Discovers and initializes the GIC hardware.
 */
void gic_init(void)
{
    struct pht_node* gic_node = pht_find_device("gic");
    if (gic_node == (void*)0)
    {
        PANIC("[  GIC ] Device node not found in hardware tree!\n");
    }

    uintptr_t gicd_vbase = P2V(gic_node->address[0]);
    uintptr_t gicc_vbase = P2V(gic_node->address[1]);

    // Map register addresses to the global pointers
    gic_d_ctlr = (volatile unsigned int*)(gicd_vbase + 0x000);
    gic_d_isenablern = (volatile unsigned int*)(gicd_vbase + 0x100);
    gic_d_ipriorityr = (volatile unsigned char*)(gicd_vbase + 0x400);
    gic_d_itargetsr = (volatile unsigned char*)(gicd_vbase + 0x800);
    gic_d_sgir = (volatile unsigned int*)(gicd_vbase + 0xF00);

    gic_c_ctlr = (volatile unsigned int*)(gicc_vbase + 0x000);
    gic_c_pmr = (volatile unsigned int*)(gicc_vbase + 0x004);
    gic_c_iar = (volatile unsigned int*)(gicc_vbase + 0x00C);
    gic_c_eoir = (volatile unsigned int*)(gicc_vbase + 0x010);

    // Enable Distributor
    mmio_write(gic_d_ctlr, 1);

    // Enable physical timer IRQ
    mmio_write(&gic_d_isenablern[GIC_TIMER_IRQ / 32], (1 << (GIC_TIMER_IRQ % 32)));
    mmio_write8(&gic_d_ipriorityr[GIC_TIMER_IRQ], 0);

    // Enable UART IRQ and route to CPU0
    cached_uart_irq = uart_get_irq();
    mmio_write(&gic_d_isenablern[cached_uart_irq / 32], (1 << (cached_uart_irq % 32)));
    mmio_write8(&gic_d_ipriorityr[cached_uart_irq], 0);
    mmio_write8(&gic_d_itargetsr[cached_uart_irq], 0x01);

    // Enable CPU Interface
    mmio_write(gic_c_ctlr, 1);
    mmio_write(gic_c_pmr, 0xFF);

    printf("[  GIC ] GIC-400 distributor @ 0x%lx, CPU interface @ 0x%lx\n", (unsigned long)gicd_vbase,
           (unsigned long)gicc_vbase);
    printf("[  GIC ] IRQ %d (phys timer) enabled, IRQ %d (UART0) -> CPU0\n", GIC_TIMER_IRQ, cached_uart_irq);
}

/*
 * gic_secondary_init - Secondary CPU interface setup.
 */
void gic_secondary_init(void)
{
    mmio_write(gic_c_ctlr, 1);
    mmio_write(gic_c_pmr, 0xFF);
    mmio_write(&gic_d_isenablern[GIC_TIMER_IRQ / 32], (1 << (GIC_TIMER_IRQ % 32)));
}
