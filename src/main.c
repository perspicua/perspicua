#include "mmio/uart.h"

int main()
{
    uart_init();
    uart_puts("Hello world!\n");
    while (1)
        ;
    return 0;
}
