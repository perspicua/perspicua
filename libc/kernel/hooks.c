#include "types.h"
#include "driver/uart.h"
#include "driver/fb_console.h"

void __libc_write(const char* buf, size_t len)
{
    uart_write(buf, len);

    for (size_t i = 0; i < len; i++)
    {
        fb_console_putc(buf[i]);
    }
}
