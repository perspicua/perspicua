#include "lib/stdio.h"
#include "driver/uart.h"
#include "kernel/lock.h"
#include <stdarg.h>

static spinlock_t printf_lock = SPINLOCK_INIT;

static void print_unsigned_long(uint64_t n, int base)
{
    char buf[64];
    int i = 0;

    if (n == 0)
    {
        uart_write("0", 1);
        return;
    }

    while (n != 0)
    {
        uint64_t remainder = n % base;

        if (remainder < 10)
            buf[i++] = (char)remainder + '0';
        else
            buf[i++] = (char)(remainder - 10) + 'a';

        n /= base;
    }

    while (i > 0)
        uart_write(&buf[--i], 1);
}

static void print_long(int64_t num, int base)
{
    if (num < 0 && base == 10)
    {
        uart_write("-", 1);
        print_unsigned_long((uint64_t)(-(num + 1)) + 1, base);
    }
    else
    {
        print_unsigned_long((uint64_t)num, base);
    }
}

static void print_number(int32_t num, int base)
{
    char buf[32];
    int i = 0;
    uint32_t n;
    if (num < 0 && base == 10)
    {
        uart_write("-", 1);
        n = (uint32_t)(-(num + 1)) + 1;
    }
    else
    {
        n = (uint32_t)num;
    }

    if (n == 0)
    {
        uart_write("0", 1);
        return;
    }

    while (n != 0)
    {
        int remainder = n % base;

        if (remainder < 10)
            buf[i++] = (char)remainder + '0';
        else
            buf[i++] = (char)(remainder - 10) + 'a';

        n /= base;
    }

    while (i > 0)
        uart_write(&buf[--i], 1);
}

void printf(const char* fmt, ...)
{
    unsigned long flags = spin_lock_irqsave(&printf_lock);
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
                unsigned int i = va_arg(args, unsigned int);
                print_unsigned_long(i, 16);
                break;
            }
            case 'p':
            {
                unsigned long i = va_arg(args, unsigned long);
                uart_write("0x", 2);
                print_unsigned_long(i, 16);
                break;
            }
            case 's':
            {
                char* s = va_arg(args, char*);
                while (*s)
                {
                    if (*s == '\n')
                        uart_write("\r", 1);
                    uart_write(s++, 1);
                }
                break;
            }
            case 'c':
            {
                char c = (char)va_arg(args, int);
                uart_write(&c, 1);
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
                    print_long(i, 10);
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
                uart_write("%", 1);
                break;
            }
            default:
            {
                uart_write("%", 1);
                uart_write(p, 1);
                break;
            }
            }
        }
        else
        {
            if (*p == '\n')
                uart_write("\r", 1);
            uart_write(p, 1);
        }
    }
    va_end(args);
    spin_unlock_irqrestore(&printf_lock, flags);
}
