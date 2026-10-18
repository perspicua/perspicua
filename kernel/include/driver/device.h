/*
 * device.h - Devicetree-driven driver model: registration by boot phase, probing, resources.
 */

#ifndef PERSPICUA_DRIVER_DEVICE_H
#define PERSPICUA_DRIVER_DEVICE_H

#include <stdint.h>

#include "devicetree/fdt.h"

struct device {
    const char *name;
    const uint32_t *fdt_node;
    void *priv;
};

struct device_driver {
    const char *name;
    const char *compatible;
    int (*probe)(struct device *dev);
};

// Registers a driver for one boot phase, through a linker section per phase.
#define CORE_DRIVER(_name) \
    static const struct device_driver _name __attribute__((used, section(".drivers.core")))

#define DEVICE_DRIVER(_name) \
    static const struct device_driver _name __attribute__((used, section(".drivers.device")))

#define IRQ_DRIVER(_name) \
    static const struct device_driver _name __attribute__((used, section(".drivers.irq")))

// Probes every driver registered for that phase; main.c calls each at its stage.
void driver_probe_core(void);
void driver_probe_irqs(void);
void driver_probe_devices(void);

uintptr_t devm_get_io_base(struct device *dev, int index);
unsigned int devm_get_irq(struct device *dev, int index);

#endif // PERSPICUA_DRIVER_DEVICE_H
