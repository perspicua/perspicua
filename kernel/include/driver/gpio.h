/*
 * gpio.h - Public API for the General Purpose Input/Output (GPIO) driver.
 *
 * This file defines the constants and functions for configuring GPIO pins,
 * including setting pin functions and pull-up/down resistors.
 */

#ifndef PERSPICUA_DRIVER_GPIO_H
#define PERSPICUA_DRIVER_GPIO_H

/* Maximum number of GPIO pins on the BCM2711 */
#define GPIO_MAX_PIN 57

/* GPIO Function Select values */
#define GPIO_FUNC_INPUT 0b000
#define GPIO_FUNC_OUTPUT 0b001
#define GPIO_FUNC_ALT0 0b100
#define GPIO_FUNC_ALT1 0b101
#define GPIO_FUNC_ALT2 0b110
#define GPIO_FUNC_ALT3 0b111
#define GPIO_FUNC_ALT4 0b011
#define GPIO_FUNC_ALT5 0b010

/* GPIO Pull-up/down resistor values */
#define GPIO_PUPDN_NONE 0b00
#define GPIO_PUPDN_UP 0b01
#define GPIO_PUPDN_DOWN 0b10

/*
 * gpio_init - Discovers and initializes the GPIO base address from the hardware tree.
 */
void gpio_init(void);

/*
 * gpio_set_pin_function - Sets the function of a specific GPIO pin.
 */
void gpio_set_pin_function(unsigned int pin, unsigned int function);

/*
 * gpio_set_pull - Configures the pull-up/down resistor for a GPIO pin.
 */
void gpio_set_pull(unsigned int pin, unsigned int pull);

#endif /* PERSPICUA_DRIVER_GPIO_H */
