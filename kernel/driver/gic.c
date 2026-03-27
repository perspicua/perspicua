/*
 * gic.c - Implementation of the GICv2 driver.
 *
 * This module handles GIC discovery from the devicetree, register mapping,
 * and interrupt routing configuration for both primary and secondary cores.
 */

#include "driver/gic.h"

#include "io.h"
#include "stdio.h"
#include "panic.h"

#include "mm/addr.h"
#include "devicetree/fdt.h"
#include "driver/uart.h"

/* --- Global Variables --- */

volatile unsigned int *gic_d_ctlr = NULL;
volatile unsigned int *gic_d_isenablern = NULL;
volatile unsigned char *gic_d_ipriorityr = NULL;
volatile unsigned char *gic_d_itargetsr = NULL;
volatile unsigned int *gic_d_sgir = NULL;

volatile unsigned int *gic_c_ctlr = NULL;
volatile unsigned int *gic_c_pmr = NULL;
volatile unsigned int *gic_c_iar = NULL;
volatile unsigned int *gic_c_eoir = NULL;

/* --- Private Variables --- */

static unsigned int cached_uart_irq = 0;

/* --- Public API Implementations --- */

/*
 * gic_send_panic_ipi - Signals all other cores using SGI 0.
 */
void gic_send_panic_ipi(void)
{
    /* Target filter 0b10: all cores except self */
    mmio_write(gic_d_sgir, (0b10 << 24) | 0);
}

/*
 * gic_init - Maps and configures the GIC distributor and primary CPU interface.
 */
void gic_init(void)
{
    const uint32_t *gic_node = fdt_find_node_by_compatible("arm,gic-400");
    if (!gic_node) {
        gic_node = fdt_find_node_by_compatible("arm,cortex-a15-gic");
        if (!gic_node) {
            PANIC("GIC: device node not found in DTB");
        }
    }

    struct fdt_property reg_prop;
    if (fdt_get_property(gic_node, "reg", &reg_prop) != 0) {
        PANIC("GIC: missing 'reg' property");
    }

    const uint32_t *reg_data = (const uint32_t *)reg_prop.value;
    uint32_t gicd_phys = fdt32_to_cpu(reg_data[0]);
    uint32_t gicc_phys = fdt32_to_cpu(reg_data[2]);

    /* Handle BCM2711 legacy address translation quirks */
    if (gicd_phys == 0x40041000) {
        gicd_phys = 0xFF841000;
        gicc_phys = 0xFF842000;
    } else if (gicd_phys < 0xFC000000 && gicc_phys < 0xFC000000) {
        gicd_phys = (gicd_phys & 0x01FFFFFF) | 0xFF800000;
        gicc_phys = (gicc_phys & 0x01FFFFFF) | 0xFF800000;
    }

    uintptr_t gicd_vbase = P2V(gicd_phys);
    uintptr_t gicc_vbase = P2V(gicc_phys);

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

    pr_info("gic: distributor @ 0x%lx, CPU @ 0x%lx\n", (unsigned long)gicd_vbase,
            (unsigned long)gicc_vbase);
}

/*
 * gic_secondary_init - Enables the local CPU interface and core-local IRQs.
 */
void gic_secondary_init(void)
{
    mmio_write(gic_c_ctlr, 1);
    mmio_write(gic_c_pmr, 0xFF);
    mmio_write(&gic_d_isenablern[GIC_TIMER_IRQ / 32], (1 << (GIC_TIMER_IRQ % 32)));
}
