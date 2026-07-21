/*
 * uart.h - Public API for the PL011 UART driver.
 *
 * This header defines constants and functions for managing serial
 * communication through the primary UART controller.
 */

#ifndef PERSPICUA_DRIVER_UART_H
#define PERSPICUA_DRIVER_UART_H

#include "types.h"

#include "core/lock.h"

/* UART Flag Register (FR) bits */
#define UART_FR_TXFF (1 << 5)
#define UART_FR_RXFE (1 << 4)
#define UART_FR_TXFE (1 << 7)

/* UART Line Control Register (LCRH) bits */
#define UART_LCRH_FEN    (1 << 4)
#define UART_LCRH_WLEN_8 (3 << 5)

/* UART Control Register (CR) bits */
#define UART_CR_UARTEN (1 << 0)
#define UART_CR_TXE    (1 << 8)
#define UART_CR_RXE    (1 << 9)

/* UART Interrupt Mask Set/Clear (IMSC) bits */
#define UART_IMSC_RXIM (1 << 4)
#define UART_IMSC_TXIM (1 << 5)
#define UART_IMSC_RTIM (1 << 6)

/* UART Masked Interrupt Status (MIS) bits */
#define UART_MIS_RXMIS (1 << 4)
#define UART_MIS_TXMIS (1 << 5)
#define UART_MIS_RTMIS (1 << 6)

extern volatile uint32_t *uart_dr;
extern volatile uint32_t *uart_fr;
extern volatile uint32_t *uart_mis;
extern volatile uint32_t *uart_imsc;
extern spinlock_t uart_tx_lock;

typedef void (*uart_rx_cb_t)(char c);
typedef void (*uart_tx_cb_t)(void);

/*
 * uart_send - Transmits a single character with full synchronization.
 */
void uart_send(char c);

/*
 * uart_send_raw - Transmits a character without locking (caller must hold lock).
 */
void uart_send_raw(char c);

/*
 * uart_getc - Receives a single character (blocking).
 */
char uart_getc(void);

/*
 * uart_puts - Sends a null-terminated string to the UART.
 */
void uart_puts(const char *str);

/*
 * uart_puts_locked - Sends a string atomically while holding the lock.
 */
void uart_puts_locked(const char *str);

/*
 * uart_data_ready - Returns non-zero if RX FIFO is not empty.
 */
int uart_data_ready(void);

/*
 * uart_enable_interrupts - Enables RX and RX Timeout interrupts.
 */
void uart_enable_interrupts(void);

/*
 * uart_clear_interrupt - Clears interrupts matching the bitmask.
 */
void uart_clear_interrupt(uint32_t mask);

/*
 * uart_get_irq - Returns the cached IRQ number for the UART.
 */
unsigned int uart_get_irq(void);

/*
 * uart_reg_rx_callback - Registers a function to be called for each received byte.
 */
void uart_reg_rx_callback(uart_rx_cb_t f);

/*
 * uart_reg_tx_callback - Registers a function to be called when the TX FIFO drains.
 */
void uart_reg_tx_callback(uart_tx_cb_t f);

/*
 * uart_handle_irq - Reads the interrupt status and dispatches to registered callbacks.
 */
void uart_handle_irq(void);

#endif /* PERSPICUA_DRIVER_UART_H */
