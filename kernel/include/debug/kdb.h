/*
 * kdb.h - Kernel Debugger (KDB) public API.
 *
 * Entered on a kernel panic, with the other cores halted. Called directly, it
 * stops only the calling core: the others keep running.
 *
 * Commands available in the KDB prompt:
 *   help              List all commands
 *   regs              Dump CPU registers
 *   sysregs           Dump key AArch64 system registers
 *   tasks             List processes and tasks
 *   mem               Show physical memory statistics
 *   bt                Print a stack backtrace
 *   rd <addr> [n]     Hex-dump n 64-bit words at addr (default 8)
 *   wr <addr> <val>   Write a 64-bit value to addr
 *   continue          Exit KDB and resume (or halt after panic)
 */

#ifndef PERSPICUA_DEBUG_KDB_H
#define PERSPICUA_DEBUG_KDB_H

#include "arch/exception.h"

/*
 * kdb_enter - Enter the kernel debugger from an arbitrary kernel context.
 *
 * Masks this core's interrupts and the UART's receive interrupt, and drops into
 * the REPL. 'continue' restores both and returns.
 */
void kdb_enter(const char *reason);

/*
 * kdb_enter_tf - Enter the kernel debugger with a saved exception frame.
 *
 * Like kdb_enter(), but the 'regs' and 'bt' commands use the register
 * state captured in the trap frame rather than the live EL1 context.
 * Used by the panic handler and exception vectors.
 */
void kdb_enter_tf(const char *reason, struct exception_trap_frame *tf);

// Reads the word at addr into *val, or returns -1 if the read faults.
int kdb_peek(const unsigned long *addr, unsigned long *val);

// Writes val to the word at addr, or returns -1 if the write faults.
int kdb_poke(unsigned long *addr, unsigned long val);

#endif // PERSPICUA_DEBUG_KDB_H
