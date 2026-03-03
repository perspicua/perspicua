#include "stdio.h"
#include "../driver/uart.h"
#include <stdarg.h>

static void print_unsigned_long(uint64_t n, int base)
{
    char buf[64];
    int i = 0;

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

static void print_number(int32_t num, int base)
{
    char buf[32];
    int i = 0;
    uint32_t n;
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

void print_unsigned_int(uint32_t n, int base)
{
    print_unsigned_long(n, base);
}
void printf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    for (const char* p = fmt; *p != '\0'; p++)
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
                unsigned long i = va_arg(args, unsigned long);
                print_unsigned_long(i, 16);
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
            case 'u':
            {
                unsigned int i = va_arg(args, unsigned int);
                print_unsigned_long(i, 10);
                break;
            }
            case 'l':
            {
                p++;
                switch (*p)
                {
                case 'd':
                {
                    long i = va_arg(args, long);
                    print_number(i, 10);
                    break;
                }
                case 'u':
                {
                    unsigned long i = va_arg(args, unsigned long);
                    print_unsigned_long(i, 10);
                    break;
                }
                case 'x':
                {
                    unsigned long i = va_arg(args, unsigned long);
                    print_unsigned_long(i, 16);
                    break;
                }
                }
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
