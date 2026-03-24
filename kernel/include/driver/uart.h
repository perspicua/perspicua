/*
 * uart.h - Public API for the PL011 UART driver.
 *
 * This file defines the constants, macros, and function prototypes for
 * managing serial communication through the primary UART controller.
 */

#ifndef PERSPICUA_DRIVER_UART_H
#define PERSPICUA_DRIVER_UART_H

#include "core/lock.h"
#include "types.h"
#include "io.h"

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

/* Public UART Register Pointers and Synchronization */
extern volatile uint32_t* uart_dr;
extern volatile uint32_t* uart_fr;
extern volatile uint32_t* uart_mis;
extern volatile uint32_t* uart_imsc;
extern spinlock_t uart_tx_lock;

/*
 * uart_init - Discovers the UART device from the hardware tree and
 * configures it for 115200 baud, 8n1, with FIFOs enabled.
 */
void uart_init(void);

/*
 * uart_send - Transmits a single character with full synchronization.
 */
void uart_send(char c);

/*
 * uart_send_raw - Transmits a single character without taking a lock.
 * Caller MUST hold uart_tx_lock.
 */
void uart_send_raw(char c);

/*
 * uart_getc - Receives a single character. This function blocks until
 * a character is available in the receive FIFO.
 */
char uart_getc(void);

/*
 * uart_puts - Sends a null-terminated string to the UART.
 */
void uart_puts(const char* str);

/*
 * uart_puts_locked - Sends a null-terminated string while holding the
 * UART spinlock to ensure atomic output across multiple cores.
 */
void uart_puts_locked(const char* str);

/*
 * uart_write - A wrapper for tty_write that routes output through the
 * console TTY subsystem.
 */
void uart_write(const char* buf, size_t len);

/*
 * uart_data_ready - Returns non-zero if there is at least one character
 * waiting in the receive FIFO.
 */
int uart_data_ready(void);

/*
 * uart_enable_interrupts - Enables the receive and receive timeout interrupts.
 */
void uart_enable_interrupts(void);

/*
 * uart_clear_interrupt - Clears the interrupts specified by the given mask.
 */
void uart_clear_interrupt(uint32_t mask);

/*
 * uart_get_irq - Returns the cached IRQ number for the UART controller.
 */
unsigned int uart_get_irq(void);

#endif /* PERSPICUA_DRIVER_UART_H */
