#include "uart.h"
#include "gpio.h"
#include "../timer.h"

#define PERIPHERAL_BASE 0xFE000000
#define UART0_BASE (PERIPHERAL_BASE + 0x201000)

volatile unsigned int* const UART0_DR = (unsigned int*)(UART0_BASE + 0x00);
volatile unsigned int* const UART0_FR = (unsigned int*)(UART0_BASE + 0x18);
volatile unsigned int* const UART0_IBRD = (unsigned int*)(UART0_BASE + 0x24);
volatile unsigned int* const UART0_FBRD = (unsigned int*)(UART0_BASE + 0x28);
volatile unsigned int* const UART0_LCRH = (unsigned int*)(UART0_BASE + 0x2C);
volatile unsigned int* const UART0_CR = (unsigned int*)(UART0_BASE + 0x30);
volatile unsigned int* const UART0_ICR = (unsigned int*)(UART0_BASE + 0x44);

void uart_init(void)
{
    // clear control register, disables uart0
    *UART0_CR = 0;

    // set pin 14 15
    gpio_set_pin_function(14, GPIO_FUNC_ALT0);
    gpio_set_pin_function(15, GPIO_FUNC_ALT0);

    // disable pull resistors
    gpio_set_pull(14, GPIO_PUPDN_NONE);
    gpio_set_pull(15, GPIO_PUPDN_NONE);

    // TODO: i am not sure if this is needed, needs testing
    sleep_ms(1);

    *UART0_ICR = 0x7FF;

    // set baud rate to 115200
    // UART clock default = 48MHz
    // 48,000,000 / (16 * 115200) = 26.04166666666666666666 etc.
    // Integer part = 26. Fractional part = int((0.0416666 * 64) + 0.5) = 3.
    *UART0_IBRD = 26;
    *UART0_FBRD = 3;

    // enable FIFOs (bit 4) and 8-bit word length (bits 5 and 6) -> 0b1110000 -> 0x70
    *UART0_LCRH = 0x70;

    // UARTEN (bit 0) | TXE (bit 8) | RXE (bit 9) -> 0b1100000001 -> 0x301
    *UART0_CR = 0x301;
}

void uart_send(char c)
{
    // wait for TXFF = 0
    // if it is 1, the buffer is full and we have to wait before sending another char
    while (*UART0_FR & (1 << 5))
        ;
    *UART0_DR = c;
}

char uart_getc(void)
{
    char r;
    // wait for RXFE = 0
    while (*UART0_FR & (1 << 4))
        ;

    r = (char)(*UART0_DR);
    if (r == '\r')
        r = '\n';
    return r;
}

void uart_puts(const char* str)
{
    while (*str)
    {
        if (*str == '\n')
            uart_send('\r');
        uart_send(*str++);
    }
}
