/*
 * gic.c - Implementation of the GICv2 driver.
 *
 * This module handles GIC discovery from the devicetree, register mapping,
 * and interrupt routing configuration for both primary and secondary cores.
 */

#include "driver/gic.h"
#include "driver/device.h"

#include "io.h"
#include "stdio.h"
#include "panic.h"

#include "mm/addr.h"
#include "devicetree/fdt.h"
#include "driver/uart.h"

volatile unsigned int *gic_d_ctlr = NULL;
volatile unsigned int *gic_d_isenablern = NULL;
volatile unsigned char *gic_d_ipriorityr = NULL;
volatile unsigned char *gic_d_itargetsr = NULL;
volatile unsigned int *gic_d_sgir = NULL;

volatile unsigned int *gic_c_ctlr = NULL;
volatile unsigned int *gic_c_pmr = NULL;
volatile unsigned int *gic_c_iar = NULL;
volatile unsigned int *gic_c_eoir = NULL;

static unsigned int cached_uart_irq = 0;

/*
 * gic_send_panic_ipi - Signals all other cores using SGI 0.
 */
void gic_send_panic_ipi(void)
{
    /* Target filter 0b10: all cores except self */
    mmio_write(gic_d_sgir, (0b10 << 24) | 0);
}

/*
 * gic_probe - Maps and configures the GIC distributor and primary CPU interface.
 */
static int gic_probe(struct device *dev)
{
    uintptr_t gicd_vbase = devm_get_io_base(dev, 0);
    uintptr_t gicc_vbase = devm_get_io_base(dev, 1);

    if (!gicd_vbase || !gicc_vbase) {
        PANIC("GIC: missing or invalid 'reg' property");
    }

    gic_d_ctlr = (volatile unsigned int *)(gicd_vbase + 0x000);
    gic_d_isenablern = (volatile unsigned int *)(gicd_vbase + 0x100);
    gic_d_ipriorityr = (volatile unsigned char *)(gicd_vbase + 0x400);
    gic_d_itargetsr = (volatile unsigned char *)(gicd_vbase + 0x800);
    gic_d_sgir = (volatile unsigned int *)(gicd_vbase + 0xF00);

    gic_c_ctlr = (volatile unsigned int *)(gicc_vbase + 0x000);
    gic_c_pmr = (volatile unsigned int *)(gicc_vbase + 0x004);
    gic_c_iar = (volatile unsigned int *)(gicc_vbase + 0x00C);
    gic_c_eoir = (volatile unsigned int *)(gicc_vbase + 0x010);

    /* Distributor: enable all IRQ forwarding */
    mmio_write(gic_d_ctlr, 1);

    /* Enable Physical Timer (PPI) */
    mmio_write(&gic_d_isenablern[GIC_TIMER_IRQ / 32], (1 << (GIC_TIMER_IRQ % 32)));
    mmio_write8(&gic_d_ipriorityr[GIC_TIMER_IRQ], 0);

    /* Enable UART (SPI) and route specifically to CPU0 */
    cached_uart_irq = uart_get_irq();
    mmio_write(&gic_d_isenablern[cached_uart_irq / 32], (1 << (cached_uart_irq % 32)));
    mmio_write8(&gic_d_ipriorityr[cached_uart_irq], 0);
    mmio_write8(&gic_d_itargetsr[cached_uart_irq], 0x01);

    /* CPU Interface: enable and allow all priority levels */
    mmio_write(gic_c_ctlr, 1);
    mmio_write(gic_c_pmr, 0xFF);

    return 0;
}

/*
 * gic_enable_irq - Enables an SPI and routes it to CPU0.
 */
void gic_enable_irq(unsigned int irq)
{
    if (!gic_d_isenablern) {
        return;
    }
    mmio_write(&gic_d_isenablern[irq / 32], (1u << (irq % 32)));
    mmio_write8(&gic_d_ipriorityr[irq], 0);
    mmio_write8(&gic_d_itargetsr[irq], 0x01);
}

IRQ_DRIVER(gic_400) = {
    .name = "gic-400",
    .compatible = "arm,gic-400",
    .probe = gic_probe,
};

IRQ_DRIVER(gic_a15) = {
    .name = "cortex-a15-gic",
    .compatible = "arm,cortex-a15-gic",
    .probe = gic_probe,
};

/*
 * gic_secondary_init - Enables the local CPU interface and core-local IRQs.
 */
void gic_secondary_init(void)
{
    mmio_write(gic_c_ctlr, 1);
    mmio_write(gic_c_pmr, 0xFF);
    mmio_write(&gic_d_isenablern[GIC_TIMER_IRQ / 32], (1 << (GIC_TIMER_IRQ % 32)));
}
