#include "driver/device.h"
#include "stdio.h"
#include "string.h"
#include "mm/heap.h"
#include "mm/addr.h"
#include "panic.h"

extern struct device_driver __drivers_core_start[];
extern struct device_driver __drivers_core_end[];

extern struct device_driver __drivers_bus_start[];
extern struct device_driver __drivers_bus_end[];

extern struct device_driver __drivers_device_start[];
extern struct device_driver __drivers_device_end[];

static void probe_driver_list(struct device_driver *start, struct device_driver *end)
{
    for (struct device_driver *drv = start; drv < end; drv++) {
        if (!drv->compatible || !drv->probe) {
            continue;
        }

        /*
         * Search the FDT for a node matching this driver's compatible string.
         * For simplicity in this initial implementation, we only probe the first matching node.
         * A complete implementation would iterate through all matching nodes.
         */
        const uint32_t *node = fdt_find_node_by_compatible(drv->compatible);
        if (node) {
            /* Create a dummy device instance for now */
            struct device dev;
            dev.name = drv->name;
            dev.fdt_node = node;
            dev.priv = NULL;

            /* Probe the driver */
            int ret = drv->probe(&dev);
            if (ret != 0) {
                pr_err("driver: %s probe failed with error %d\n", drv->name, ret);
            } else {
                pr_info("driver: %s probed successfully\n", drv->name);
            }
        }
    }
}

void driver_probe_core(void)
{
    pr_info("driver: probing CORE level drivers...\n");
    probe_driver_list(__drivers_core_start, __drivers_core_end);
}

void driver_probe_bus(void)
{
    pr_info("driver: probing BUS level drivers...\n");
    probe_driver_list(__drivers_bus_start, __drivers_bus_end);
}

void driver_probe_devices(void)
{
    pr_info("driver: probing DEVICE level drivers...\n");
    probe_driver_list(__drivers_device_start, __drivers_device_end);
}

/*
 * devm_get_io_base - Retrieves and maps the MMIO base address for a device.
 * index specifies which 'reg' entry to retrieve (usually 0).
 */
uintptr_t devm_get_io_base(struct device *dev, int index)
{
    if (!dev || !dev->fdt_node) {
        return 0;
    }

    struct fdt_property reg_prop;
    if (fdt_get_property(dev->fdt_node, "reg", &reg_prop) != 0) {
        return 0;
    }

    const uint32_t *reg_data = (const uint32_t *)reg_prop.value;
    /*
     * A very simplified assumption for now: if size is >= 12, it might be
     * a 64-bit address, so we take the second 32-bit cell. Otherwise the first.
     * This logic matches the original UART/GPIO code, but a true implementation
     * should read #address-cells and #size-cells from the parent node.
     */
    uint32_t phys_base =
        (reg_prop.size >= 12 && index == 0)
            ? fdt32_to_cpu(reg_data[1])
            : fdt32_to_cpu(reg_data[index * 2]); /* Very rudimentary index support */

    /* Handle legacy BCM address translation if needed */
    if (phys_base < 0xFC000000) {
        phys_base = (phys_base & 0x01FFFFFF) | 0xFE000000;
    }

    return P2V(phys_base);
}

/*
 * devm_get_irq - Retrieves the IRQ number for a device.
 * index specifies which 'interrupts' entry to retrieve (usually 0).
 */
unsigned int devm_get_irq(struct device *dev, int index)
{
    if (!dev || !dev->fdt_node) {
        return 0;
    }

    struct fdt_property irq_prop;
    if (fdt_get_property(dev->fdt_node, "interrupts", &irq_prop) != 0) {
        return 0;
    }

    const uint32_t *irq_data = (const uint32_t *)irq_prop.value;

    /*
     * simplified assumptions again matching original code.
     * ARM GIC bindings typically have 3 cells: type, number, flags.
     */
    uint32_t type = fdt32_to_cpu(irq_data[index * 3]);
    uint32_t num = fdt32_to_cpu(irq_data[index * 3 + 1]);

    return (type == 0) ? num + 32 : num;
}
