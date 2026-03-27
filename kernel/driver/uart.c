/*
 * uart.c - Implementation of the PL011 UART driver.
 *
 * This module handles register-level UART configuration, discovery
 * from the DTB, and synchronized I/O operations.
 */

#include "driver/uart.h"

#include "io.h"
#include "stdio.h"
#include "types.h"

#include "core/tty.h"
#include "core/lock.h"
#include "core/timer.h"
#include "panic.h"
#include "mm/addr.h"
#include "devicetree/fdt.h"
#include "driver/gpio.h"

extern struct tty console_tty;

/* --- Global Variables --- */

spinlock_t uart_tx_lock = SPINLOCK_INIT;
int uart_ready = 0;

/* Public Register Pointers */
volatile uint32_t *uart_dr = NULL;
volatile uint32_t *uart_fr = NULL;
volatile uint32_t *uart_mis = NULL;
volatile uint32_t *uart_imsc = NULL;

/* --- Private Variables --- */

static unsigned int cached_uart_irq = 0;

/* Private Register Pointers */
static volatile uint32_t *uart_ibrd = NULL;
static volatile uint32_t *uart_fbrd = NULL;
static volatile uint32_t *uart_lcrh = NULL;
static volatile uint32_t *uart_cr = NULL;
static volatile uint32_t *uart_ifls = NULL;
static volatile uint32_t *uart_icr = NULL;

/* --- Public API Implementations --- */

/*
 * uart_init - Locates the PL011 UART and initializes it for 8n1 operation.
 */
void uart_init(void)
{
    const uint32_t *uart_node = fdt_find_node_by_compatible("arm,pl011-axi");
    if (!uart_node) {
        PANIC("UART: device node not found");
    }

    struct fdt_property reg_prop;
    if (fdt_get_property(uart_node, "reg", &reg_prop) != 0) {
        PANIC("UART: missing 'reg' property");
    }

    const uint32_t *reg_data = (const uint32_t *)reg_prop.value;
    uint32_t phys_base =
        (reg_prop.size >= 12) ? fdt32_to_cpu(reg_data[1]) : fdt32_to_cpu(reg_data[0]);

    if (phys_base < 0xFC000000) {
        phys_base = (phys_base & 0x01FFFFFF) | 0xFE000000;
    }

    uintptr_t vbase = P2V(phys_base);

    /* Map register offsets */
    uart_dr = (uint32_t *)(vbase + 0x00);
    uart_fr = (uint32_t *)(vbase + 0x18);
    uart_ibrd = (uint32_t *)(vbase + 0x24);
    uart_fbrd = (uint32_t *)(vbase + 0x28);
    uart_lcrh = (uint32_t *)(vbase + 0x2C);
    uart_cr = (uint32_t *)(vbase + 0x30);
    uart_ifls = (uint32_t *)(vbase + 0x34);
    uart_imsc = (uint32_t *)(vbase + 0x38);
    uart_mis = (uint32_t *)(vbase + 0x40);
    uart_icr = (uint32_t *)(vbase + 0x44);

    /* Disable UART before programming */
    mmio_write(uart_cr, 0);

    /* Configure GPIO 14 & 15 for UART Alt0 */
    gpio_set_pin_function(14, GPIO_FUNC_ALT0);
    gpio_set_pin_function(15, GPIO_FUNC_ALT0);
    gpio_set_pull(14, GPIO_PUPDN_NONE);
    gpio_set_pull(15, GPIO_PUPDN_NONE);

    sleep_ms(10);
    mmio_write(uart_icr, 0x7FF); /* Clear all interrupts */

    /* Baud rate calculation for 115200 (based on 48MHz clock) */
    mmio_write(uart_ibrd, 26);
    mmio_write(uart_fbrd, 3);
    mmio_write(uart_lcrh, UART_LCRH_FEN | UART_LCRH_WLEN_8);
    mmio_write(uart_ifls, 0);
    mmio_write(uart_cr, UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE);

    uart_ready = 1;

    struct fdt_property irq_prop;
    if (fdt_get_property(uart_node, "interrupts", &irq_prop) == 0) {
        const uint32_t *irq_data = (const uint32_t *)irq_prop.value;
        uint32_t type = fdt32_to_cpu(irq_data[0]);
        uint32_t num = fdt32_to_cpu(irq_data[1]);
        cached_uart_irq = (type == 0) ? num + 32 : num;
    } else {
        cached_uart_irq = 153; /* Fallback for BCM2711 */
    }

    pr_info("uart: PL011 at 0x%lx, IRQ %u\n", vbase, cached_uart_irq);
}

/*
 * uart_write - Directs output to the line-buffered TTY.
 */
void uart_write(const char *buf, size_t len)
{
    tty_write(&console_tty, buf, len);
}

/*
 * uart_send_raw - Spin-waits for space and transmits a byte.
 */
void uart_send_raw(char c)
{
    while (mmio_read(uart_fr) & UART_FR_TXFF) {
        asm volatile("nop");
    }
    mmio_write(uart_dr, (unsigned int)c);
}

/*
 * uart_send - Safe, synchronized single-byte transmission.
 */
void uart_send(char c)
{
    unsigned long flags = spin_lock_irqsave(&uart_tx_lock);
    uart_send_raw(c);
    spin_unlock_irqrestore(&uart_tx_lock, flags);
}

/*
 * uart_puts_locked - Transmits a string atomically with respect to other cores.
 */
void uart_puts_locked(const char *str)
{
    unsigned long flags = spin_lock_irqsave(&uart_tx_lock);
    while (*str) {
        uart_send_raw(*str++);
    }
    spin_unlock_irqrestore(&uart_tx_lock, flags);
}

/*
 * uart_getc - Blocks until a byte is received.
 */
char uart_getc(void)
{
    while (mmio_read(uart_fr) & UART_FR_RXFE) {
        asm volatile("nop");
    }
    return (char)(mmio_read(uart_dr) & 0xFF);
}

/*
 * uart_puts - Standard string transmission.
 */
void uart_puts(const char *str)
{
    while (*str) {
        uart_send(*str++);
    }
}

/*
 * uart_data_ready - Returns 1 if at least one byte is in the RX FIFO.
 */
int uart_data_ready(void)
{
    return !(mmio_read(uart_fr) & UART_FR_RXFE);
}

/*
 * uart_enable_interrupts - Unmasks standard RX interrupts.
 */
void uart_enable_interrupts(void)
{
    mmio_write(uart_imsc, UART_IMSC_RXIM | UART_IMSC_RTIM);
}

/*
 * uart_clear_interrupt - Resets specific interrupt status bits.
 */
void uart_clear_interrupt(uint32_t mask)
{
    mmio_write(uart_icr, mask);
}

/*
 * uart_get_irq - Returns the interrupt line mapped to this UART.
 */
unsigned int uart_get_irq(void)
{
    return cached_uart_irq;
}
