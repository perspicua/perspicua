/*
 * gpio.c - Implementation of the General Purpose Input/Output (GPIO) driver.
 *
 * This file handles register-level GPIO configuration for the BCM2711,
 * including function selection and pull-up/down resistor settings.
 */

#include "driver/gpio.h"

#include "panic.h"
#include "mm/addr.h"

#include "devicetree/fdt.h"

#include "stdio.h"
/* GPIO Register Pointers (Static) */
static volatile unsigned int* gpio_gpfsel0 = (void*)0;
static volatile unsigned int* gpio_gppupdn0 = (void*)0;

/*
 * gpio_init - Discovers and initializes the GPIO base address from the hardware tree.
 */
void gpio_init(void)
{
    const uint32_t* gpio_node = fdt_find_node_by_compatible("brcm,bcm2711-gpio");
    if (!gpio_node)
    {
        PANIC("[ GPIO ] Device node not found in DTB!\n");
    }

    struct fdt_property reg_prop;
    if (fdt_get_property(gpio_node, "reg", &reg_prop) != 0)
    {
        PANIC("[ GPIO ] Missing 'reg' property in DTB!\n");
    }

    const uint32_t* reg_data = (const uint32_t*)reg_prop.value;
    uint32_t phys_base = fdt32_to_cpu(reg_data[0]);
    if (phys_base < 0xFC000000)
    {
        phys_base = (phys_base & 0x01FFFFFF) | 0xFE000000;
    }

    uintptr_t vbase = P2V(phys_base);

    // BCM2711 GPIO register offsets
    gpio_gpfsel0 = (unsigned int*)(vbase + 0x00);
    gpio_gppupdn0 = (unsigned int*)(vbase + 0xE4);

    printf("[ GPIO ] BCM2711 GPIO driver initialized (base 0x%lx)\n", vbase);
}

/*
 * gpio_set_pin_function - Sets the function of a specific GPIO pin.
 */
void gpio_set_pin_function(unsigned int pin, unsigned int function)
{
    if (pin > GPIO_MAX_PIN)
    {
        return;
    }

    unsigned int reg_index = pin / 10;
    unsigned int bit_offset = (pin % 10) * 3;
    unsigned int current_val = gpio_gpfsel0[reg_index];

    // Each pin function is represented by 3 bits
    current_val &= ~(0b111 << bit_offset);
    current_val |= (function << bit_offset);
    gpio_gpfsel0[reg_index] = current_val;
}

/*
 * gpio_set_pull - Configures the pull-up/down resistor for a GPIO pin.
 */
void gpio_set_pull(unsigned int pin, unsigned int pull)
{
    if (pin > GPIO_MAX_PIN)
    {
        return;
    }

    unsigned int reg_index = pin / 16;
    unsigned int bit_offset = (pin % 16) * 2;
    unsigned int current_val = gpio_gppupdn0[reg_index];

    // Each pin pull state is represented by 2 bits; clear then set
    current_val &= ~(0b11 << bit_offset);
    current_val |= (pull << bit_offset);

    gpio_gppupdn0[reg_index] = current_val;
}
