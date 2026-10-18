/*
 * uart.c - Driver for the PL011 PrimeCell UART.
 */

#include "driver/uart.h"

#include <stddef.h>
#include <stdint.h>

#include "stdio.h"

#include "uapi/errno.h"

#include "arch/irq.h"

#include "core/lock.h"
#include "core/timer.h"
#include "devicetree/fdt.h"
#include "driver/device.h"
#include "driver/gic.h"
#include "driver/gpio.h"
#include "io.h"
#include "mm/addr.h"
#include "panic.h"

spinlock_t uart_tx_lock = SPINLOCK_INIT;
int uart_ready = 0;

static volatile uint32_t *uart_dr = NULL;
volatile uint32_t *uart_fr = NULL;
static volatile uint32_t *uart_mis = NULL;
static volatile uint32_t *uart_imsc = NULL;

static unsigned int cached_uart_irq = 0;

static volatile uint32_t *uart_ibrd = NULL;
static volatile uint32_t *uart_fbrd = NULL;
static volatile uint32_t *uart_lcrh = NULL;
static volatile uint32_t *uart_cr = NULL;
static volatile uint32_t *uart_ifls = NULL;
static volatile uint32_t *uart_icr = NULL;

// Registered interrupt callbacks
static uart_rx_cb_t uart_rx_callback = NULL;
static uart_tx_cb_t uart_tx_callback = NULL;
static uart_flush_cb_t uart_flush_callback = NULL;

// Under uart_tx_lock: kernel messages part-way out, which the tty ring waits behind.
static int uart_tx_writers;

// Indexed by the DR error bit less 8: framing, parity, break, overrun.
static unsigned long uart_rx_error_counts[4];

static void uart_handle_irq(void);

static irq_result_t uart_irq_handler(void *ctx)
{
    (void)ctx;
    uart_handle_irq();
    return IRQ_HANDLED;
}

static int pl011_uart_probe(struct device *dev)
{
    if (uart_ready) {
        return -EEXIST;
    }

    uintptr_t vbase = devm_get_io_base(dev, 0);
    if (!vbase) {
        PANIC("UART: missing or invalid 'reg' property");
    }

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

    // The PL011 stays disabled while it is programmed.
    mmio_write(uart_cr, 0);

    // Configure GPIO 14 & 15 for UART Alt0
    gpio_set_pin_function(14, GPIO_FUNC_ALT0);
    gpio_set_pin_function(15, GPIO_FUNC_ALT0);
    gpio_set_pull(14, GPIO_PUPDN_NONE);
    gpio_set_pull(15, GPIO_PUPDN_NONE);

    timer_sleep_ms(10);
    mmio_write(uart_icr, 0x7FF);

    // Baud rate calculation for 115200 (based on 48MHz clock)
    mmio_write(uart_ibrd, 26);
    mmio_write(uart_fbrd, 3);
    mmio_write(uart_lcrh, UART_LCRH_FEN | UART_LCRH_WLEN_8);
    mmio_write(uart_ifls, 0);
    mmio_write(uart_cr, UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE);

    uart_ready = 1;

    cached_uart_irq = devm_get_irq(dev, 0);
    if (!cached_uart_irq) {
        cached_uart_irq = 153; // Fallback for BCM2711
    }

    int err = request_irq(cached_uart_irq, uart_irq_handler, NULL, "uart");
    if (err != 0) {
        pr_warn("uart: could not claim IRQ %u; input will not work\n", cached_uart_irq);
    }

    return 0;
}

CORE_DRIVER(pl011_uart) = {
    .name = "pl011-uart",
    .compatible = "arm,pl011-axi",
    .probe = pl011_uart_probe,
};

void uart_send_raw(char c)
{
    while (mmio_read(uart_fr) & UART_FR_TXFF) {
        asm volatile("nop");
    }
    mmio_write(uart_dr, (unsigned int)c);
}

static void uart_write_raw(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uart_send_raw(buf[i]);
    }
}

// A panicking core must not take uart_tx_lock.
static inline int uart_tx_unserialized(void)
{
    return kernel_panicked;
}

void uart_send(char c)
{
    uart_write_locked(&c, 1);
}

int uart_tx_paused_locked(void)
{
    return uart_tx_writers > 0;
}

void uart_tx_irq_locked(int on)
{
    uint32_t mask = mmio_read(uart_imsc);
    mmio_write(uart_imsc, on ? (mask | UART_IMSC_TXIM) : (mask & ~UART_IMSC_TXIM));
}

// After the queued tty output, a FIFO at a time: IRQs are masked for the register writes only.
void uart_write_locked(const char *buf, size_t len)
{
    if (uart_tx_unserialized()) {
        uart_write_raw(buf, len);
        return;
    }

    unsigned long flags = spin_lock_irqsave(&uart_tx_lock);
    uart_tx_writers++;

    int ring_left = uart_flush_callback != NULL;
    size_t i = 0;
    for (;;) {
        if (ring_left) {
            ring_left = uart_flush_callback();
        }
        while (!ring_left && i < len && !(mmio_read(uart_fr) & UART_FR_TXFF)) {
            mmio_write(uart_dr, (unsigned int)buf[i++]);
        }
        if (!ring_left && i == len) {
            break;
        }

        spin_unlock_irqrestore(&uart_tx_lock, flags);
        while (mmio_read(uart_fr) & UART_FR_TXFF) {
            asm volatile("yield");
        }
        flags = spin_lock_irqsave(&uart_tx_lock);
    }

    // The tty ring stood still for this message; its interrupt starts it again.
    if (--uart_tx_writers == 0 && uart_tx_callback) {
        uart_tx_irq_locked(1);
    }
    spin_unlock_irqrestore(&uart_tx_lock, flags);
}

char uart_getc(void)
{
    while (mmio_read(uart_fr) & UART_FR_RXFE) {
        asm volatile("nop");
    }
    return (char)(mmio_read(uart_dr) & 0xFF);
}

void uart_enable_interrupts(void)
{
    unsigned long flags = spin_lock_irqsave(&uart_tx_lock);
    mmio_write(uart_imsc, mmio_read(uart_imsc) | UART_IMSC_RXIM | UART_IMSC_RTIM);
    spin_unlock_irqrestore(&uart_tx_lock, flags);
}

uint32_t uart_rx_irq_save(void)
{
    // A panic may have halted the core holding uart_tx_lock.
    int locked = !kernel_panicked;
    unsigned long flags = locked ? spin_lock_irqsave(&uart_tx_lock) : 0;
    uint32_t mask = mmio_read(uart_imsc);
    mmio_write(uart_imsc, mask & ~(UART_IMSC_RXIM | UART_IMSC_RTIM));
    if (locked) {
        spin_unlock_irqrestore(&uart_tx_lock, flags);
    }
    return mask & (UART_IMSC_RXIM | UART_IMSC_RTIM);
}

void uart_rx_irq_restore(uint32_t saved)
{
    int locked = !kernel_panicked;
    unsigned long flags = locked ? spin_lock_irqsave(&uart_tx_lock) : 0;
    mmio_write(uart_imsc, mmio_read(uart_imsc) | saved);
    if (locked) {
        spin_unlock_irqrestore(&uart_tx_lock, flags);
    }
}

void uart_get_rx_errors(struct uart_rx_errors *out)
{
    out->framing = __atomic_load_n(&uart_rx_error_counts[0], __ATOMIC_RELAXED);
    out->parity = __atomic_load_n(&uart_rx_error_counts[1], __ATOMIC_RELAXED);
    out->brk = __atomic_load_n(&uart_rx_error_counts[2], __ATOMIC_RELAXED);
    out->overrun = __atomic_load_n(&uart_rx_error_counts[3], __ATOMIC_RELAXED);
}

static void uart_clear_interrupt(uint32_t mask)
{
    mmio_write(uart_icr, mask);
}

void uart_reg_rx_callback(uart_rx_cb_t f)
{
    uart_rx_callback = f;
}

void uart_reg_tx_callback(uart_tx_cb_t f)
{
    uart_tx_callback = f;
}

void uart_reg_flush_callback(uart_flush_cb_t f)
{
    uart_flush_callback = f;
}

static void uart_handle_irq(void)
{
    uint32_t mis = mmio_read(uart_mis);

    // Acknowledged before draining: a byte after the final RXFE check raises its own interrupt.
    uart_clear_interrupt(mis);

    if (mis & (UART_MIS_RXMIS | UART_MIS_RTMIS)) {
        while (!(mmio_read(uart_fr) & UART_FR_RXFE)) {
            uint32_t dr = mmio_read(uart_dr);
            for (int bit = 0; bit < 4; bit++) {
                if (dr & (1u << (8 + bit))) {
                    __atomic_fetch_add(&uart_rx_error_counts[bit], 1, __ATOMIC_RELAXED);
                }
            }
            if (uart_rx_callback != NULL) {
                uart_rx_callback((char)(dr & 0xFF));
            }
        }
    }
    if (mis & UART_MIS_TXMIS) {
        if (uart_tx_callback != NULL) {
            uart_tx_callback();
        }
    }
}
