/*
 * uart.c - Implementation of the PL011 UART driver.
 *
 * This file handles register-level UART configuration, discovery from
 * the hardware tree, and synchronous transmit/receive operations.
 */

#include "driver/uart.h"

#include "types.h"
#include "tty.h"
#include "lock.h"
#include "timer.h"
#include "panic.h"
#include "addr.h"

#include "devicetree/pht.h"

#include "driver/gpio.h"

/* Extern console TTY from the TTY subsystem */
extern struct tty console_tty;

/* UART State and Synchronization */
static spinlock_t uart_lock         = SPINLOCK_INIT;
static unsigned int cached_uart_irq = 0;

/* Public Register Pointers */
volatile uint32_t* uart_dr   = (void*)0;
volatile uint32_t* uart_fr   = (void*)0;
volatile uint32_t* uart_mis  = (void*)0;
volatile uint32_t* uart_imsc = (void*)0;

/* Private Register Pointers (Static) */
static volatile uint32_t* uart_ibrd = (void*)0;
static volatile uint32_t* uart_fbrd = (void*)0;
static volatile uint32_t* uart_lcrh = (void*)0;
static volatile uint32_t* uart_cr   = (void*)0;
static volatile uint32_t* uart_ifls = (void*)0;
static volatile uint32_t* uart_icr  = (void*)0;

/*
 * uart_init - Configures the UART for standard serial communication.
 */
void uart_init(void)
{
    struct pht_node* uart_node = pht_find_device("uart");
    if (uart_node == (void*)0)
    {
        PANIC("[ UART ] Device node not found in hardware tree!\n");
    }

    uintptr_t vbase = P2V(uart_node->address[0]);

    // Map hardware offsets to pointers
    uart_dr   = (uint32_t*)(vbase + 0x00);
    uart_fr   = (uint32_t*)(vbase + 0x18);
    uart_ibrd = (uint32_t*)(vbase + 0x24);
    uart_fbrd = (uint32_t*)(vbase + 0x28);
    uart_lcrh = (uint32_t*)(vbase + 0x2C);
    uart_cr   = (uint32_t*)(vbase + 0x30);
    uart_ifls = (uint32_t*)(vbase + 0x34);
    uart_imsc = (uint32_t*)(vbase + 0x38);
    uart_mis  = (uint32_t*)(vbase + 0x40);
    uart_icr  = (uint32_t*)(vbase + 0x44);

    // Disable UART for safe configuration
    mmio_write(uart_cr, 0);

    // Configure GPIO pins 14 and 15 for ALT0 function (UART)
    gpio_set_pin_function(14, GPIO_FUNC_ALT0);
    gpio_set_pin_function(15, GPIO_FUNC_ALT0);

    // Disable pull-up/down resistors for cleaner signal
    gpio_set_pull(14, GPIO_PUPDN_NONE);
    gpio_set_pull(15, GPIO_PUPDN_NONE);

    // Small delay for hardware stability
    sleep_ms(10);

    // Clear all pending interrupts
    mmio_write(uart_icr, 0x7FF);

    // Set baud rate to 115200 for 48MHz clock.
    // 48,000,000 / (16 * 115200) = 26.04166
    mmio_write(uart_ibrd, 26);
    mmio_write(uart_fbrd, 3);

    // Enable FIFOs and set 8-bit word length
    mmio_write(uart_lcrh, UART_LCRH_FEN | UART_LCRH_WLEN_8);

    // Set FIFO interrupt levels to 1/8 to trigger early
    mmio_write(uart_ifls, 0);

    // Enable UART, TX, and RX
    mmio_write(uart_cr, UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE);

    cached_uart_irq = uart_node->irq;
}

/*
 * uart_write - Proxies write calls to the console TTY.
 */
void uart_write(const char* buf, size_t len)
{
    tty_write(&console_tty, buf, len);
}

/*
 * uart_puts_locked - Atomically transmits a string with IRQs disabled.
 */
void uart_puts_locked(const char* str)
{
    unsigned long flags = spin_lock_irqsave(&uart_lock);
    while (*str)
    {
        uart_send(*str++);
    }
    spin_unlock_irqrestore(&uart_lock, flags);
}

/*
 * uart_send - Transmits a single byte when space is available.
 */
void uart_send(char c)
{
    // Wait for the transmit FIFO to have room
    while (mmio_read(uart_fr) & UART_FR_TXFF)
    {
        asm volatile("nop");
    }
    mmio_write(uart_dr, (unsigned int)c);
}

/*
 * uart_getc - Receives a single byte, blocking until available.
 */
char uart_getc(void)
{
    // Wait for the receive FIFO to be non-empty
    while (mmio_read(uart_fr) & UART_FR_RXFE)
    {
        asm volatile("nop");
    }

    return (char)(mmio_read(uart_dr) & 0xFF);
}

/*
 * uart_puts - Transmits a null-terminated string.
 */
void uart_puts(const char* str)
{
    while (*str)
    {
        uart_send(*str++);
    }
}

/*
 * uart_data_ready - Checks if any characters are waiting in the RX buffer.
 */
int uart_data_ready(void)
{
    return !(mmio_read(uart_fr) & UART_FR_RXFE);
}

/*
 * uart_enable_interrupts - Enables hardware interrupts for RX and RX Timeout.
 */
void uart_enable_interrupts(void)
{
    mmio_write(uart_imsc, UART_IMSC_RXIM | UART_IMSC_RTIM);
}

/*
 * uart_clear_interrupt - Clears UART interrupts by their bitmask.
 */
void uart_clear_interrupt(uint32_t mask)
{
    mmio_write(uart_icr, mask);
}

/*
 * uart_get_irq - Returns the interrupt line assigned to the UART.
 */
unsigned int uart_get_irq(void)
{
    return cached_uart_irq;
}
