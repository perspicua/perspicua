/*
 * gpio.h - Public API for the General Purpose Input/Output (GPIO) driver.
 *
 * This header defines constants and functions for configuring BCM2711
 * GPIO pins, including function selection and pull-up/down resistors.
 */

#ifndef PERSPICUA_DRIVER_GPIO_H
#define PERSPICUA_DRIVER_GPIO_H

#define GPIO_MAX_PIN 57

/* GPIO Function Select values (3 bits per pin) */
#define GPIO_FUNC_INPUT  0b000
#define GPIO_FUNC_OUTPUT 0b001
#define GPIO_FUNC_ALT0   0b100
#define GPIO_FUNC_ALT1   0b101
#define GPIO_FUNC_ALT2   0b110
#define GPIO_FUNC_ALT3   0b111
#define GPIO_FUNC_ALT4   0b011
#define GPIO_FUNC_ALT5   0b010

/* GPIO Pull-up/down values (2 bits per pin) */
#define GPIO_PUPDN_NONE 0b00
#define GPIO_PUPDN_UP   0b01
#define GPIO_PUPDN_DOWN 0b10

/*
 * gpio_set_pin_function - Sets the operational mode of a GPIO pin.
 */
void gpio_set_pin_function(unsigned int pin, unsigned int function);

/*
 * gpio_set_pull - Configures the internal pull resistor for a GPIO pin.
 */
void gpio_set_pull(unsigned int pin, unsigned int pull);

#endif /* PERSPICUA_DRIVER_GPIO_H */
