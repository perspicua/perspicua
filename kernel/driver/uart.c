/*
 * uart.c - Implementation of the PL011 UART driver.
 *
 * This file handles register-level UART configuration, discovery from
 * the hardware tree, and synchronous transmit/receive operations.
 */

#include "driver/uart.h"

#include "types.h"
#include "core/tty.h"
#include "core/lock.h"
#include "core/timer.h"
#include "panic.h"
#include "mm/addr.h"

#include "devicetree/fdt.h"

#include "driver/gpio.h"

#include "stdio.h"
/* Extern console TTY from the TTY subsystem */
extern struct tty console_tty;

/* UART State and Synchronization */
static spinlock_t uart_lock = SPINLOCK_INIT;
static unsigned int cached_uart_irq = 0;
int uart_ready = 0;

/* Public Register Pointers */
volatile uint32_t* uart_dr = NULL;
volatile uint32_t* uart_fr = NULL;
volatile uint32_t* uart_mis = NULL;
volatile uint32_t* uart_imsc = NULL;

/* Private Register Pointers (Static) */
static volatile uint32_t* uart_ibrd = NULL;
static volatile uint32_t* uart_fbrd = NULL;
static volatile uint32_t* uart_lcrh = NULL;
static volatile uint32_t* uart_cr = NULL;
static volatile uint32_t* uart_ifls = NULL;
static volatile uint32_t* uart_icr = NULL;

/*
 * uart_init - Configures the UART for standard serial communication.
 */
void uart_init(void)
{
    const uint32_t* uart_node = fdt_find_node_by_compatible("arm,pl011-axi");
    if (!uart_node)
    {
        PANIC("[ UART ] Device node not found in DTB!\n");
    }

    struct fdt_property reg_prop;
    if (fdt_get_property(uart_node, "reg", &reg_prop) != 0)
    {
        PANIC("[ UART ] Missing 'reg' property in DTB!\n");
    }

    const uint32_t* reg_data = (const uint32_t*)reg_prop.value;
    uint32_t phys_base;

    /*
     * RPi4 DTB often uses different #address-cells. We determine the
     * correct address cell by looking at the property size.
     * 8 bytes:  (addr32, size32)
     * 12 bytes: (addr64, size32)
     * 16 bytes: (addr64, size64)
     */
    if (reg_prop.size >= 12)
    {
        phys_base = fdt32_to_cpu(reg_data[1]);
    }
    else
    {
        phys_base = fdt32_to_cpu(reg_data[0]);
    }

    if (phys_base < 0xFC000000)
    {
        // Broadcom uses legacy 0x7E... addresses in DTB for compatibility, we must map them
        // to RPi4's actual 0xFE... bus addresses. Usually DTBs have a `ranges` under /soc,
        // but as a hardcoded workaround to ease migration for now:
        phys_base = (phys_base & 0x01FFFFFF) | 0xFE000000;
    }

    uintptr_t vbase = P2V(phys_base);

    // Map hardware offsets to pointers
    uart_dr = (uint32_t*)(vbase + 0x00);
    uart_fr = (uint32_t*)(vbase + 0x18);
    uart_ibrd = (uint32_t*)(vbase + 0x24);
    uart_fbrd = (uint32_t*)(vbase + 0x28);
    uart_lcrh = (uint32_t*)(vbase + 0x2C);
    uart_cr = (uint32_t*)(vbase + 0x30);
    uart_ifls = (uint32_t*)(vbase + 0x34);
    uart_imsc = (uint32_t*)(vbase + 0x38);
    uart_mis = (uint32_t*)(vbase + 0x40);
    uart_icr = (uint32_t*)(vbase + 0x44);

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

    uart_ready = 1;

    struct fdt_property irq_prop;
    if (fdt_get_property(uart_node, "interrupts", &irq_prop) == 0)
    {
        const uint32_t* irq_data = (const uint32_t*)irq_prop.value;
        // GIC interrupts in DTB often have 3 cells [type, number, flags].
        // For SPIs (type 0), we usually add 32 to get the actual IRQ number.
        uint32_t type = fdt32_to_cpu(irq_data[0]);
        uint32_t num = fdt32_to_cpu(irq_data[1]);
        if (type == 0)
        {  // SPI
            cached_uart_irq = num + 32;
        }
        else
        {
            cached_uart_irq = num;
        }
    }
    else
    {
        // Fallback for RPi4 PL011 if interrupts property missing or different
        cached_uart_irq = 153;  // 121 SPI + 32
    }

    printf("[ UART ] PL011 UART initialized (base 0x%lx, IRQ %u)\n", vbase, cached_uart_irq);
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
