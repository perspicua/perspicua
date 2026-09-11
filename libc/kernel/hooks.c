/*
 * hooks.c - Kernel-mode libc glue logic.
 */

#include "types.h"
#include "driver/uart.h"

// Routes string data to the kernel UART driver.
void __libc_write(const char *buf, size_t len)
{
    if (!uart_ready) {
        return;
    }

    uart_write_locked(buf, len);
}
