#include "uart.h"
#include "gpio.h"
#include "../kernel/timer.h"
#include "../kernel/addr.h"
#include "../kernel/lock.h"
#include "../lib/panic.h"
#include "../devicetree/pht.h"

static spinlock_t uart_lock = SPINLOCK_INIT;

volatile unsigned int* UART0_DR;
// ... (rest of pointers)
volatile unsigned int* UART0_FR;
volatile unsigned int* UART0_IBRD;
volatile unsigned int* UART0_FBRD;
volatile unsigned int* UART0_LCRH;
volatile unsigned int* UART0_CR;
volatile unsigned int* UART0_IMSC;
volatile unsigned int* UART0_ICR;

void uart_init(void)
{
    struct pht_node* uart_node = pht_find_device("uart");
    if (uart_node == NULL)
    {
        PANIC("[ UART ] Device node not found in hardware tree!\n");
    }

    uintptr_t vbase = P2V(uart_node->address[0]);

    UART0_FR = (unsigned int*)(vbase + 0x18);
    UART0_IBRD = (unsigned int*)(vbase + 0x24);
    UART0_FBRD = (unsigned int*)(vbase + 0x28);
    UART0_LCRH = (unsigned int*)(vbase + 0x2C);
    UART0_CR = (unsigned int*)(vbase + 0x30);
    UART0_IMSC = (unsigned int*)(vbase + 0x38);
    UART0_ICR = (unsigned int*)(vbase + 0x44);
    UART0_DR = (unsigned int*)(vbase + 0x00);

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

void uart_write(const char* buf, size_t len)
{
    unsigned long flags = spin_lock_irqsave(&uart_lock);
    for (size_t i = 0; i < len; i++)
    {
        if (buf[i] == '\n')
            uart_send('\r');
        uart_send(buf[i]);
    }
    spin_unlock_irqrestore(&uart_lock, flags);
}

void uart_puts_locked(const char* str)
{
    unsigned long flags = spin_lock_irqsave(&uart_lock);
    while (*str)
    {
        if (*str == '\n')
            uart_send('\r');
        uart_send(*str++);
    }
    spin_unlock_irqrestore(&uart_lock, flags);
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

int uart_data_ready(void)
{
    return !(*UART0_FR & (1 << 4));
}

void uart_enable_interrupts(void)
{
    *UART0_IMSC |= (1 << 4) | (1 << 6);
}

void uart_clear_interrupt(void)
{
    *UART0_ICR = (1 << 4) | (1 << 6);
}

unsigned int uart_get_irq(void)
{
    struct pht_node* uart_node = pht_find_device("uart");
    if (uart_node == NULL)
    {
        PANIC("[ UART ] Device node not found in hardware tree!\n");
    }
    return uart_node->irq;
}
