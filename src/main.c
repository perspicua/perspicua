#include "stdio.h"
#include "mmio/uart.h"

int main()
{
    uart_init();
    printf("Hello World!\n");
    printf("Running tests...\n");

    printf("Testing string   : %s\n", "It works!");
    printf("Testing char     : %c\n", 'A');
    printf("Testing positive : %d\n", 67);
    printf("Testing negative : %d\n", -676767);
    printf("Testing hex      : %x\n", 255);
    printf("Testing pointers : %x\n", &main);
    printf("Testing percent  : %d%%\n", 100);

    while (1)
    {
        char c = uart_getc();
        uart_send(c);
        if (c == '\n')
            uart_send('\r');
    }
    return 0;
}
