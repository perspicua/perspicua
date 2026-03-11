#include "panic.h"
#include "stdio.h"
#include "timer.h"
#include "driver/gic.h"

volatile int kernel_panicked = 0;

void panic(const char* msg, const char* file, int line)
{
    disable_interrupts();

    kernel_panicked = 1;
    // ensure the write is visible to all cores before the IPI lands
    asm volatile("dsb ish" ::: "memory");

    // wake all other cores so they see kernel_panicked and halt
    gic_send_panic_ipi();

    printf("\n\n*** KERNEL PANIC ***\n");
    printf("  %s\n", msg);
    printf("  at %s:%d\n", file, line);
    printf("  All cores halted.\n");

    for (;;)
        asm volatile("wfe");
}
