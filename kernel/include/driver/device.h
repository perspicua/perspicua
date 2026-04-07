#ifndef PERSPICUA_DRIVER_DEVICE_H
#define PERSPICUA_DRIVER_DEVICE_H

#include "types.h"
#include "devicetree/fdt.h"

/*
 * Represents a hardware device instantiated from the Devicetree.
 */
struct device {
    const char *name;
    const uint32_t *fdt_node; /* Pointer to the device's FDT node */
    void *priv;               /* Driver-private data */
};

/*
 * Represents a device driver that can bind to a device.
 */
struct device_driver {
    const char *name;
    const char *compatible; /* Devicetree compatible string to match */
    int (*probe)(struct device *dev);
};

/*
 * Macros to register drivers at different boot phases via linker sections.
 */
#define CORE_DRIVER(_name) \
    static const struct device_driver _name __attribute__((used, section(".drivers.core")))

#define BUS_DRIVER(_name) \
    static const struct device_driver _name __attribute__((used, section(".drivers.bus")))
#define DEVICE_DRIVER(_name) \
    static const struct device_driver _name __attribute__((used, section(".drivers.device")))

#define IRQ_DRIVER(_name) \
    static const struct device_driver _name __attribute__((used, section(".drivers.irq")))

/*
 * Probes all drivers registered at the specified level.
 * Call these from main.c at appropriate initialization stages.
 */
void driver_probe_core(void);
void driver_probe_bus(void);
void driver_probe_irqs(void);
void driver_probe_devices(void);

/*
 * Device Resource Management
 */
uintptr_t devm_get_io_base(struct device *dev, int index);
unsigned int devm_get_irq(struct device *dev, int index);

#endif /* PERSPICUA_DRIVER_DEVICE_H */
