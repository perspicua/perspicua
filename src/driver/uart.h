#ifndef _UART_H_
#define _UART_H_

#include "../lib/types.h"

void uart_init(void);
void uart_send(char c);
char uart_getc(void);
void uart_puts(const char* str);
void uart_puts_locked(const char* str);
void uart_write(const char* buf, size_t len);
int uart_data_ready(void);
void uart_enable_interrupts(void);
void uart_clear_interrupt(void);
unsigned int uart_get_irq(void);

#endif // _UART_H_
