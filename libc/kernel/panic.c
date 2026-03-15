/*
 * panic.c - Implementation of the kernel panic handler.
 *
 * This file contains the logic for gracefully halting the system when an
 * unrecoverable kernel error is detected, including notification of
 * other CPU cores.
 */

#include "panic.h"

#include "stdio.h"
#include "timer.h"

#include "driver/gic.h"

/* Global state for core synchronization during panic */
volatile int kernel_panicked = 0;

/*
 * panic - Primary kernel error handler.
 */
void panic(const char* msg, const char* file, int line)
{
    /* Immediately mask local interrupts */
    disable_interrupts();

    kernel_panicked = 1;

    /* Ensure the panic state is globally visible before signaling other cores */
    asm volatile("dsb ish" ::: "memory");

    /* Broadcast a Software Generated Interrupt to halt secondary cores */
    gic_send_panic_ipi();

    printf("\n\n*** KERNEL PANIC ***\n");
    printf("  Message : %s\n", msg);
    printf("  Location: %s:%d\n", file, line);
    printf("  All CPU cores halted.\n");

    /* Infinite halt loop */
    while (1)
    {
        asm volatile("wfe");
    }
}
