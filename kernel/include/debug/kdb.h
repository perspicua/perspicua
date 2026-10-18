/*
 * kdb.h - Kernel debugger. `help` at its prompt lists the commands.
 */

#ifndef PERSPICUA_DEBUG_KDB_H
#define PERSPICUA_DEBUG_KDB_H

#include "arch/exception.h"

// Masks this core's IRQs and the UART's RX interrupt until `continue`; other cores keep running.
void kdb_enter(const char *reason);

// As kdb_enter, but regs and bt read the trap frame instead of the live EL1 context.
void kdb_enter_tf(const char *reason, struct exception_trap_frame *tf);

// Reads the word at addr into *val, or returns -1 if the read faults.
int kdb_peek(const unsigned long *addr, unsigned long *val);

// Writes val to the word at addr, or returns -1 if the write faults.
int kdb_poke(unsigned long *addr, unsigned long val);

#endif // PERSPICUA_DEBUG_KDB_H
