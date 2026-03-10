#ifndef _UART_H_
#define _UART_H_

#include "lib/types.h"
#include "lib/io.h"

// UART register bits
#define UART_FR_TXFF (1 << 5)
#define UART_FR_RXFE (1 << 4)
#define UART_FR_TXFE (1 << 7)

#define UART_LCRH_FEN (1 << 4)
#define UART_LCRH_WLEN_8 (3 << 5)

#define UART_CR_UARTEN (1 << 0)
#define UART_CR_TXE (1 << 8)
#define UART_CR_RXE (1 << 9)

#define UART_IMSC_RXIM (1 << 4)
#define UART_IMSC_TXIM (1 << 5)
#define UART_IMSC_RTIM (1 << 6)

#define UART_MIS_RXMIS (1 << 4)
#define UART_MIS_TXMIS (1 << 5)
#define UART_MIS_RTMIS (1 << 6)

extern volatile uint32_t* UART0_DR;
extern volatile uint32_t* UART0_FR;
extern volatile uint32_t* UART0_MIS;
extern volatile uint32_t* UART0_IMSC;

void uart_init(void);
void uart_send(char c);
char uart_getc(void);
void uart_puts(const char* str);
void uart_puts_locked(const char* str);
void uart_write(const char* buf, size_t len);
int uart_data_ready(void);
void uart_enable_interrupts(void);
void uart_clear_interrupt(uint32_t mask);
unsigned int uart_get_irq(void);

#endif // _UART_H_
