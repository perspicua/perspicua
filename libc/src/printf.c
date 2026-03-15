/*
 * printf.c - Implementation of the formatted output engine.
 *
 * This file contains the primary printf logic, including support for
 * decimal, hexadecimal, pointer, character, and string formatting.
 * It uses a pluggable __libc_write backend for both kernel and user-mode.
 */

#include "stdio.h"

#include <stdarg.h>

#include "types.h"

#ifdef __KERNEL__
#include "lock.h"
/* Global synchronization for kernel-mode console output */
static spinlock_t printf_lock = SPINLOCK_INIT;
#endif

/* External write hook provided by the kernel or user glue code */
extern void __libc_write(const char* buf, size_t len);

/*
 * print_unsigned_long - Formats and outputs an unsigned 64-bit integer.
 */
static void print_unsigned_long(uint64_t n, int base)
{
    char buf[64];
    int i = 0;

    if (n == 0)
    {
        __libc_write("0", 1);
        return;
    }

    while (n != 0)
    {
        uint64_t remainder = n % base;
        if (remainder < 10)
        {
            buf[i++] = (char)remainder + '0';
        }
        else
        {
            buf[i++] = (char)(remainder - 10) + 'a';
        }
        n /= base;
    }

    /* Output the digits in correct (reverse) order */
    while (i > 0)
    {
        __libc_write(&buf[--i], 1);
    }
}

/*
 * print_long - Formats and outputs a signed 64-bit integer.
 */
static void print_long(int64_t num, int base)
{
    if (num < 0 && base == 10)
    {
        __libc_write("-", 1);
        /* Handle minimum value overflow by working with absolute magnitude */
        print_unsigned_long((uint64_t)(-(num + 1)) + 1, base);
    }
    else
    {
        print_unsigned_long((uint64_t)num, base);
    }
}

/*
 * print_number - Formats and outputs a signed 32-bit integer.
 */
static void print_number(int32_t num, int base)
{
    char buf[32];
    int i = 0;
    uint32_t n;

    if (num < 0 && base == 10)
    {
        __libc_write("-", 1);
        n = (uint32_t)(-(num + 1)) + 1;
    }
    else
    {
        n = (uint32_t)num;
    }

    if (n == 0)
    {
        __libc_write("0", 1);
        return;
    }

    while (n != 0)
    {
        int remainder = n % base;
        if (remainder < 10)
        {
            buf[i++] = (char)remainder + '0';
        }
        else
        {
            buf[i++] = (char)(remainder - 10) + 'a';
        }
        n /= base;
    }

    while (i > 0)
    {
        __libc_write(&buf[--i], 1);
    }
}

/*
 * printf - Formatted output implementation.
 */
void printf(const char* fmt, ...)
{
#ifdef __KERNEL__
    unsigned long flags = spin_lock_irqsave(&printf_lock);
#endif

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
                __libc_write("0x", 2);
                print_unsigned_long(i, 16);
                break;
            }
            case 's':
            {
                char* s = va_arg(args, char*);
                while (s && *s)
                {
                    if (*s == '\n')
                    {
                        __libc_write("\r", 1);
                    }
                    __libc_write(s++, 1);
                }
                break;
            }
            case 'c':
            {
                char c = (char)va_arg(args, int);
                __libc_write(&c, 1);
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
                __libc_write("%", 1);
                break;
            }
            default:
            {
                __libc_write("%", 1);
                __libc_write(p, 1);
                break;
            }
            }
        }
        else
        {
            if (*p == '\n')
            {
                __libc_write("\r", 1);
            }
            __libc_write(p, 1);
        }
    }

    va_end(args);

#ifdef __KERNEL__
    spin_unlock_irqrestore(&printf_lock, flags);
#endif
}
