#include "mmio/uart.h"

int main()
{
    uart_init();
    uart_puts("Hello world!\n");
    while (1)
    {
        char c = uart_getc();
        uart_send(c);
        if (c == '\n')
            uart_send('\r');
    }
    return 0;
}
