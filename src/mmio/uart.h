#ifndef _UART_H_
#define _UART_H_

void uart_init(void);
void uart_send(char c);
char uart_getc(void);
void uart_puts(const char* str);

#endif
