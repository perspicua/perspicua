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
#include "mm/addr.h"

#include "devicetree/fdt.h"

#include "driver/uart.h"

/* Distributor registers */
volatile unsigned int* gic_d_ctlr = NULL;
volatile unsigned int* gic_d_isenablern = NULL;
volatile unsigned char* gic_d_ipriorityr = NULL;
volatile unsigned char* gic_d_itargetsr = NULL;
volatile unsigned int* gic_d_sgir = NULL;

/* CPU Interface registers */
volatile unsigned int* gic_c_ctlr = NULL;
volatile unsigned int* gic_c_pmr = NULL;
volatile unsigned int* gic_c_iar = NULL;
volatile unsigned int* gic_c_eoir = NULL;

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
    const uint32_t* gic_node = fdt_find_node_by_compatible("arm,gic-400");
    if (!gic_node)
    {
        // Try alternate compatible string used on some RPi4 DTBs
        gic_node = fdt_find_node_by_compatible("arm,cortex-a15-gic");
        if (!gic_node)
        {
            PANIC("[  GIC ] Device node not found in DTB!\n");
        }
    }

    struct fdt_property reg_prop;
    if (fdt_get_property(gic_node, "reg", &reg_prop) != 0)
    {
        PANIC("[  GIC ] Missing 'reg' property in DTB!\n");
    }

    const uint32_t* reg_data = (const uint32_t*)reg_prop.value;

    // GIC usually has two memory regions defined in `reg`:
    // reg = <gicd_base size gicc_base size> or similar depending on #address-cells = 2 or 1
    // We assume 32-bit address cells for simplicity here (or legacy mappings).
    uint32_t gicd_phys = fdt32_to_cpu(reg_data[0]);
    // Usually size is in reg_data[1] if 4-word array, so gicc is at index 2
    uint32_t gicc_phys = fdt32_to_cpu(reg_data[2]);

    // Some RPi4 firmware DTB maps it correctly. Usually GICD is 0xff841000 and GICC is 0xff842000
    // But since the Broadcom legacy map uses 0x40000000 in DTB, handle it:
    if (gicd_phys == 0x40041000)
    {
        gicd_phys = 0xFF841000;
        gicc_phys = 0xFF842000;
    }
    // Workaround for BCM legacy address translation for GIC (ARM local peripherals)
    else if (gicd_phys < 0xFC000000 && gicc_phys < 0xFC000000)
    {
        gicd_phys = (gicd_phys & 0x01FFFFFF) | 0xFF800000;
        gicc_phys = (gicc_phys & 0x01FFFFFF) | 0xFF800000;
    }

    uintptr_t gicd_vbase = P2V(gicd_phys);
    uintptr_t gicc_vbase = P2V(gicc_phys);

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

    printf("[  GIC ] GIC-400 distributor @ 0x%lx, CPU interface @ 0x%lx\n",
           (unsigned long)gicd_vbase,
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
