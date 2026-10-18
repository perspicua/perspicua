/*
 * device.c - Driver registry and devicetree probing, with MMIO and IRQ lookups per device.
 */

#include "driver/device.h"

#include <stddef.h>
#include <stdint.h>

#include "stdio.h"
#include "string.h"

#include "uapi/errno.h"

#include "mm/addr.h"
#include "mm/heap.h"
#include "panic.h"

// Bus addresses the DTB uses: the ARM-local block at 0x40000000, and the VideoCore's 0x7E window.
#define ARM_LOCAL_BUS_BASE  0x40000000UL
#define ARM_LOCAL_BUS_END   0x41000000UL
#define ARM_LOCAL_PHYS_BASE 0xFF800000UL
#define VC_PERIPH_PHYS_BASE 0xFE000000UL

extern struct device_driver __drivers_core_start[];
extern struct device_driver __drivers_core_end[];

extern struct device_driver __drivers_irq_start[];
extern struct device_driver __drivers_irq_end[];

extern struct device_driver __drivers_device_start[];
extern struct device_driver __drivers_device_end[];

static void probe_driver_list(struct device_driver *start, struct device_driver *end)
{
    for (struct device_driver *drv = start; drv < end; drv++) {
        if (!drv->compatible || !drv->probe) {
            continue;
        }

        // Only the first matching node is probed, with a device that lives for the probe call only.
        const uint32_t *node = fdt_find_node_by_compatible(drv->compatible);
        if (node) {
            struct device dev;
            dev.name = drv->name;
            dev.fdt_node = node;
            dev.priv = NULL;

            int ret = drv->probe(&dev);
            if (ret != 0) {
                // ENOENT: not my hardware. EEXIST: another driver already took it.
                if (ret != -ENOENT && ret != -EEXIST) {
                    pr_err("driver: %s probe failed with error %d\n", drv->name, ret);
                }
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

void driver_probe_irqs(void)
{
    pr_info("driver: probing IRQ level drivers...\n");
    probe_driver_list(__drivers_irq_start, __drivers_irq_end);
}

void driver_probe_devices(void)
{
    pr_info("driver: probing DEVICE level drivers...\n");
    probe_driver_list(__drivers_device_start, __drivers_device_end);
}

// The mapped MMIO base of the index-th reg entry, or 0.
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

    // The parent's #address-cells and #size-cells size each reg entry; 1 each when absent.
    uint32_t address_cells = 1;
    uint32_t size_cells = 1;

    const uint32_t *parent = fdt_get_parent_node(dev->fdt_node);
    if (parent) {
        struct fdt_property ac_prop;
        if (fdt_get_property(parent, "#address-cells", &ac_prop) == 0) {
            address_cells = fdt32_to_cpu(*(const uint32_t *)ac_prop.value);
        }

        struct fdt_property sc_prop;
        if (fdt_get_property(parent, "#size-cells", &sc_prop) == 0) {
            size_cells = fdt32_to_cpu(*(const uint32_t *)sc_prop.value);
        }
    }

    uint32_t entry_cells = address_cells + size_cells;
    uint32_t offset = index * entry_cells;

    if ((offset + address_cells) * 4 > reg_prop.size) {
        return 0;
    }

    uint64_t phys_base = 0;
    if (address_cells == 1) {
        phys_base = fdt32_to_cpu(reg_data[offset]);
    } else if (address_cells == 2) {
        phys_base =
            ((uint64_t)fdt32_to_cpu(reg_data[offset]) << 32) | fdt32_to_cpu(reg_data[offset + 1]);
    } else {
        return 0; // Unsupported #address-cells size
    }

    if (phys_base >= ARM_LOCAL_BUS_BASE && phys_base < ARM_LOCAL_BUS_END) {
        phys_base = (phys_base & 0x00FFFFFF) | ARM_LOCAL_PHYS_BASE;
    } else if (phys_base < BCM2711_PERIPH_BASE) {
        phys_base = (phys_base & 0x01FFFFFF) | VC_PERIPH_PHYS_BASE;
    }

    return P2V((uintptr_t)phys_base);
}

// The index-th interrupt's GIC ID, or 0.
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

    // ARM GIC bindings typically have 3 cells: type, number, flags.
    if ((uint32_t)(index * 3 + 2) * 4 > irq_prop.size) {
        return 0;
    }

    uint32_t type = fdt32_to_cpu(irq_data[index * 3]);
    uint32_t num = fdt32_to_cpu(irq_data[index * 3 + 1]);

    return (type == 0) ? num + 32 : num;
}
