/*
 * uart.h - Public API for the PL011 UART driver.
 */

#ifndef PERSPICUA_DRIVER_UART_H
#define PERSPICUA_DRIVER_UART_H

#include "types.h"

#include "core/lock.h"

// UART Flag Register (FR) bits
#define UART_FR_TXFF (1 << 5)
#define UART_FR_RXFE (1 << 4)
#define UART_FR_TXFE (1 << 7)

// UART Line Control Register (LCRH) bits
#define UART_LCRH_FEN    (1 << 4)
#define UART_LCRH_WLEN_8 (3 << 5)

// UART Control Register (CR) bits
#define UART_CR_UARTEN (1 << 0)
#define UART_CR_TXE    (1 << 8)
#define UART_CR_RXE    (1 << 9)

// UART Interrupt Mask Set/Clear (IMSC) bits
#define UART_IMSC_RXIM (1 << 4)
#define UART_IMSC_TXIM (1 << 5)
#define UART_IMSC_RTIM (1 << 6)

// UART Masked Interrupt Status (MIS) bits
#define UART_MIS_RXMIS (1 << 4)
#define UART_MIS_TXMIS (1 << 5)
#define UART_MIS_RTMIS (1 << 6)

extern volatile uint32_t *uart_dr;
extern volatile uint32_t *uart_fr;
extern volatile uint32_t *uart_mis;
extern volatile uint32_t *uart_imsc;
extern spinlock_t uart_tx_lock;

// Set once the UART is programmed and safe to write to.
extern int uart_ready;

typedef void (*uart_rx_cb_t)(char c);
typedef void (*uart_tx_cb_t)(void);

void uart_send(char c);

void uart_send_raw(char c);

char uart_getc(void);

void uart_write_locked(const char *buf, size_t len);

int uart_data_ready(void);

void uart_enable_interrupts(void);

void uart_clear_interrupt(uint32_t mask);

unsigned int uart_get_irq(void);

void uart_reg_rx_callback(uart_rx_cb_t f);

void uart_reg_tx_callback(uart_tx_cb_t f);

void uart_handle_irq(void);

#endif // PERSPICUA_DRIVER_UART_H
