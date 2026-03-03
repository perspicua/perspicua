#include "panic.h"
#include "stdio.h"

void panic(const char* msg, const char* file, int line)
{
    asm volatile("msr daifset, #2");

    printf("\n\n*** KERNEL PANIC ***\n");
    printf("  %s\n", msg);
    printf("  at %s:%d\n", file, line);

    // low power wait
    // halts the cpu
    for (;;)
        asm volatile("wfe");
}
