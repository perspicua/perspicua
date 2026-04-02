/*
 * gpio.c - Implementation of the BCM2711 GPIO driver.
 *
 * This module handles register-level configuration, including pin function
 * selection and pull-up/down settings using the GPPUPDN registers.
 */

#include "driver/gpio.h"
#include "driver/device.h"

#include "stdio.h"
#include "panic.h"

#include "mm/addr.h"
#include "devicetree/fdt.h"

static volatile unsigned int *gpio_gpfsel0 = NULL;
static volatile unsigned int *gpio_gppupdn0 = NULL;

/*
 * bcm2711_gpio_probe - Discovers and maps the GPIO registers from the DTB.
 */
static int bcm2711_gpio_probe(struct device *dev)
{
    struct fdt_property reg_prop;
    if (fdt_get_property(dev->fdt_node, "reg", &reg_prop) != 0) {
        PANIC("GPIO: missing 'reg' property");
    }

    const uint32_t *reg_data = (const uint32_t *)reg_prop.value;
    uint32_t phys_base = fdt32_to_cpu(reg_data[0]);

    /* Handle legacy BCM address translation if needed */
    if (phys_base < 0xFC000000) {
        phys_base = (phys_base & 0x01FFFFFF) | 0xFE000000;
    }

    uintptr_t vbase = P2V(phys_base);

    /* BCM2711 specific register offsets */
    gpio_gpfsel0 = (unsigned int *)(vbase + 0x00);
    gpio_gppupdn0 = (unsigned int *)(vbase + 0xE4);

    return 0;
}

CORE_DRIVER(bcm2711_gpio) = {
    .name = "bcm2711-gpio",
    .compatible = "brcm,bcm2711-gpio",
    .probe = bcm2711_gpio_probe,
};

/*
 * gpio_set_pin_function - Sets the 3-bit function code for a GPIO pin.
 */
void gpio_set_pin_function(unsigned int pin, unsigned int function)
{
    if (pin > GPIO_MAX_PIN) {
        return;
    }

    unsigned int reg_index = pin / 10;
    unsigned int bit_offset = (pin % 10) * 3;
    unsigned int current_val = gpio_gpfsel0[reg_index];

    current_val &= ~(0b111 << bit_offset);
    current_val |= (function << bit_offset);
    gpio_gpfsel0[reg_index] = current_val;
}

/*
 * gpio_set_pull - Configures the 2-bit pull state for a GPIO pin.
 */
void gpio_set_pull(unsigned int pin, unsigned int pull)
{
    if (pin > GPIO_MAX_PIN) {
        return;
    }

    unsigned int reg_index = pin / 16;
    unsigned int bit_offset = (pin % 16) * 2;
    unsigned int current_val = gpio_gppupdn0[reg_index];

    current_val &= ~(0b11 << bit_offset);
    current_val |= (pull << bit_offset);

    gpio_gppupdn0[reg_index] = current_val;
}
