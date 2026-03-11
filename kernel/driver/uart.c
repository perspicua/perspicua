#include "driver/uart.h"
#include "driver/gpio.h"
#include "timer.h"
#include "addr.h"
#include "lock.h"
#include "tty.h"
#include "panic.h"
#include "devicetree/pht.h"

extern struct tty console_tty;

static spinlock_t uart_lock = SPINLOCK_INIT;
static unsigned int cached_uart_irq = 0;

volatile unsigned int* UART0_DR;
volatile unsigned int* UART0_FR;
volatile unsigned int* UART0_IBRD;
volatile unsigned int* UART0_FBRD;
volatile unsigned int* UART0_LCRH;
volatile unsigned int* UART0_CR;
volatile unsigned int* UART0_IFLS;
volatile unsigned int* UART0_IMSC;
volatile unsigned int* UART0_MIS;
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
    UART0_IFLS = (unsigned int*)(vbase + 0x34);
    UART0_IMSC = (unsigned int*)(vbase + 0x38);
    UART0_MIS = (unsigned int*)(vbase + 0x40);
    UART0_ICR = (unsigned int*)(vbase + 0x44);
    UART0_DR = (unsigned int*)(vbase + 0x00);

    // clear control register, disables uart0
    mmio_write(UART0_CR, 0);

    // set pin 14 15
    gpio_set_pin_function(14, GPIO_FUNC_ALT0);
    gpio_set_pin_function(15, GPIO_FUNC_ALT0);

    // disable pull resistors
    gpio_set_pull(14, GPIO_PUPDN_NONE);
    gpio_set_pull(15, GPIO_PUPDN_NONE);

    // TODO: i am not sure if this is needed, needs testing
    sleep_ms(1);

    mmio_write(UART0_ICR, 0x7FF);

    // set baud rate to 115200
    // UART clock default = 48MHz
    // 48,000,000 / (16 * 115200) = 26.04166666666666666666 etc.
    // Integer part = 26. Fractional part = int((0.0416666 * 64) + 0.5) = 3.
    mmio_write(UART0_IBRD, 26);
    mmio_write(UART0_FBRD, 3);

    // enable FIFOs (bit 4) and 8-bit word length (bits 5 and 6)
    mmio_write(UART0_LCRH, UART_LCRH_FEN | UART_LCRH_WLEN_8);

    // Interrupt FIFO level: RX 1/8, TX 1/8 (0b000 << 3 | 0b000) -> 0
    mmio_write(UART0_IFLS, 0);

    // UARTEN (bit 0) | TXE (bit 8) | RXE (bit 9)
    mmio_write(UART0_CR, UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE);

    cached_uart_irq = uart_node->irq;
}

void uart_write(const char* buf, size_t len)
{
    tty_write(&console_tty, buf, len);
}

void uart_puts_locked(const char* str)
{
    unsigned long flags = spin_lock_irqsave(&uart_lock);
    while (*str)
    {
        uart_send(*str++);
    }
    spin_unlock_irqrestore(&uart_lock, flags);
}

void uart_send(char c)
{
    // wait for TXFF = 0
    // if it is 1, the buffer is full and we have to wait before sending another char
    while (mmio_read(UART0_FR) & UART_FR_TXFF)
        ;
    mmio_write(UART0_DR, (unsigned int)c);
}

char uart_getc(void)
{
    char r;
    // wait for RXFE = 0
    while (mmio_read(UART0_FR) & UART_FR_RXFE)
        ;

    r = (char)(mmio_read(UART0_DR) & 0xFF);
    return r;
}

void uart_puts(const char* str)
{
    while (*str)
    {
        uart_send(*str++);
    }
}

int uart_data_ready(void)
{
    return !(mmio_read(UART0_FR) & UART_FR_RXFE);
}

void uart_enable_interrupts(void)
{
    mmio_write(UART0_IMSC, UART_IMSC_RXIM | UART_IMSC_RTIM);
}

void uart_clear_interrupt(uint32_t mask)
{
    mmio_write(UART0_ICR, mask);
}

unsigned int uart_get_irq(void)
{
    return cached_uart_irq;
}
