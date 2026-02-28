#include "stdio.h"
#include "../driver/uart.h"
#include <stdarg.h>

static void print_number(int num, int base)
{
    char buf[32];
    int i = 0;
    unsigned int n;
    if (num < 0 && base == 10)
    {
        uart_send('-');
        n = -num;
    }
    else
    {
        n = num;
    }

    if (n == 0)
    {
        uart_send('0');
        return;
    }

    while (n != 0)
    {
        int remainder = n % base;

        if (remainder < 10)
            buf[i++] = remainder + '0';
        else
            buf[i++] = (remainder - 10) + 'a';

        n /= base;
    }

    while (i > 0)
        uart_send(buf[--i]);
}
void printf(char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    for (char* p = fmt; *p != '\0'; p++)
    {
        if (*p == '%')
        {
            p++;

            switch (*p)
            {
            case 'd':
            {
                int i = va_arg(args, int);
                print_number(i, 10);
                break;
            }
            case 'x':
            {
                int i = va_arg(args, int);
                uart_puts("0x");
                print_number(i, 16);
                break;
            }
            case 's':
            {
                char* s = va_arg(args, char*);
                uart_puts(s);
                break;
            }
            case 'c':
            {
                int c = va_arg(args, int);
                uart_send(c);
                break;
            }
            case '%':
            {
                uart_send('%');
                break;
            }
            // TODO: add more types
            default:
            {
                uart_send('%');
                uart_send(*p);
                break;
            }
            }
        }
        else
        {
            if (*p == '\n')
                uart_send('\r');
            uart_send(*p);
        }
    }
    va_end(args);
}
