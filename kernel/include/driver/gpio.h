#ifndef _GPIO_H_
#define _GPIO_H_

#define GPIO_MAX_PIN 57

#define GPIO_FUNC_INPUT 0b000
#define GPIO_FUNC_OUTPUT 0b001
#define GPIO_FUNC_ALT0 0b100
#define GPIO_FUNC_ALT1 0b101
#define GPIO_FUNC_ALT2 0b110
#define GPIO_FUNC_ALT3 0b111
#define GPIO_FUNC_ALT4 0b011
#define GPIO_FUNC_ALT5 0b010

// pull resistor values
#define GPIO_PUPDN_NONE 0b00
#define GPIO_PUPDN_UP 0b01
#define GPIO_PUPDN_DOWN 0b10

void gpio_init(void);
void gpio_set_pin_function(unsigned int pin, unsigned int function);
void gpio_set_pull(unsigned int pin, unsigned int pull);

#endif // _GPIO_H_
