#include "exception.h"
#include "stdio.h"

void c_exception_handler()
{
    unsigned int esr;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));

    unsigned long elr;
    asm volatile("mrs %0, elr_el1" : "=r"(elr));

    printf("An unhandled exception occurred!\n");
    printf("ESR_EL1 (Reason)  : 0x%x\n", esr);
    printf("ELR_EL1 (Address) : 0x%x\n", elr);
    printf("System halted.\n");

    while (1)
    {
        asm volatile("wfe");
    }
}
