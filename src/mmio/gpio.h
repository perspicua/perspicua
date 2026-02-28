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

void gpio_set_pin_function(unsigned int pin, unsigned int function);
void gpio_set_pull(unsigned int pin, unsigned int pull);

#endif // _GPIO_H_
