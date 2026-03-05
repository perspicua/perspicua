#ifndef _UART_H_
#define _UART_H_

void uart_init(void);
void uart_send(char c);
char uart_getc(void);
void uart_puts(const char* str);
int uart_data_ready(void);
void uart_enable_interrupts(void);
void uart_clear_interrupt(void);
unsigned int uart_get_irq(void);
#endif // _UART_H_
