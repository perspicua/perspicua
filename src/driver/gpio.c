#include "gpio.h"

#define PERIPHERAL_BASE 0xFFFFFF80FE000000ULL
#define GPIO_BASE (PERIPHERAL_BASE + 0x200000)

volatile unsigned int* const GPIO_GPFSEL0 = (unsigned int*)(GPIO_BASE + 0x00);
volatile unsigned int* const GPIO_GPPUPDN0 = (unsigned int*)(GPIO_BASE + 0xE4);

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
