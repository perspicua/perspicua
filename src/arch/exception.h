#ifndef _EXCEPTION_H_
#define _EXCEPTION_H_

#include "../lib/types.h"

struct trap_frame
{
    uintptr_t elr_el1; // addr to return after syscall
    uint64_t spsr_el1; // cpu state

    // registers
    uint64_t x[30]; // x0 - x29
    uint64_t x30;   // link
} __attribute__((aligned(16)));

void c_unhandled_vector(void);
void c_irq_handler(void);
void c_sync_handler(struct trap_frame* tf);

#endif
