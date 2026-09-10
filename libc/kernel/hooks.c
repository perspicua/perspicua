/*
 * hooks.c - Kernel-mode libc glue logic.
 */

#include "types.h"
#include "driver/uart.h"

// Primary system UART readiness flag.
extern int uart_ready;

// Routes string data to the kernel UART driver.
void __libc_write(const char *buf, size_t len)
{
    if (!uart_ready) {
        return;
    }

    uart_write_locked(buf, len);
}
