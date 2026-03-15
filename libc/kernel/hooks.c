/*
 * hooks.c - Kernel-mode libc glue logic.
 *
 * This file implements the output hooks for the kernel-mode version
 * of libc, routing printf output to the primary system UART.
 */

#include "types.h"

#include "driver/uart.h"

/*
 * __libc_write - Routes string data to the kernel UART driver.
 */
void __libc_write(const char* buf, size_t len)
{
    uart_write(buf, len);
}
