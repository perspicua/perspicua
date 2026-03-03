#include "panic.h"
#include "stdio.h"
#include "../kernel/timer.h"

void panic(const char* msg, const char* file, int line)
{
    disable_interrupts();
    printf("\n\n*** KERNEL PANIC ***\n");
    printf("  %s\n", msg);
    printf("  at %s:%d\n", file, line);

    // low power wait
    // halts the cpu
    for (;;)
        asm volatile("wfe");
}
