/*
 * gpio.c - Driver for the BCM2835/BCM2711 General Purpose I/O controller.
 */

#include "driver/gpio.h"
#include "driver/device.h"

#include "stdio.h"
#include "panic.h"

#include "mm/addr.h"
#include "devicetree/fdt.h"

static volatile unsigned int *gpio_gpfsel0 = NULL;
static volatile unsigned int *gpio_gppupdn0 = NULL;

static int bcm2711_gpio_probe(struct device *dev)
{
    uintptr_t vbase = devm_get_io_base(dev, 0);
    if (!vbase) {
        PANIC("GPIO: missing or invalid 'reg' property");
    }

    // BCM2711 specific register offsets
    gpio_gpfsel0 = (unsigned int *)(vbase + 0x00);
    gpio_gppupdn0 = (unsigned int *)(vbase + 0xE4);

    return 0;
}

CORE_DRIVER(bcm2711_gpio) = {
    .name = "bcm2711-gpio",
    .compatible = "brcm,bcm2711-gpio",
    .probe = bcm2711_gpio_probe,
};

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
