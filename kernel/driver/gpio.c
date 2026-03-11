#include "driver/gpio.h"
#include "devicetree/pht.h"
#include "panic.h"
#include "addr.h"

volatile unsigned int* GPIO_GPFSEL0;
volatile unsigned int* GPIO_GPPUPDN0;

void gpio_init(void)
{
    struct pht_node* gpio_node = pht_find_device("gpio");
    if (gpio_node == NULL)
    {
        PANIC("[ GPIO ] Device node not found in hardware tree!\n");
    }

    uintptr_t vbase = P2V(gpio_node->address[0]);

    GPIO_GPFSEL0 = (unsigned int*)(vbase + 0x00);
    GPIO_GPPUPDN0 = (unsigned int*)(vbase + 0xE4);
}

void gpio_set_pin_function(unsigned int pin, unsigned int function)
{
    if (pin > GPIO_MAX_PIN)
    {
        return;
    }

    unsigned int reg_index = pin / 10;
    unsigned int bit_offset = (pin % 10) * 3;
    unsigned int current_val = GPIO_GPFSEL0[reg_index];

    current_val &= ~(0b111 << bit_offset);
    current_val |= (function << bit_offset);
    GPIO_GPFSEL0[reg_index] = current_val;
}

void gpio_set_pull(unsigned int pin, unsigned int pull)
{
    if (pin > GPIO_MAX_PIN)
    {
        return;
    }

    unsigned int reg_index = pin / 16;
    unsigned int bit_offset = (pin % 16) * 2;
    unsigned int current_val = GPIO_GPPUPDN0[reg_index];

    // clear 2 bits then set new pull state
    current_val &= ~(0b11 << bit_offset);
    current_val |= (pull << bit_offset);

    GPIO_GPPUPDN0[reg_index] = current_val;
}
