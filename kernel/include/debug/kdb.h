/*
 * kdb.h - Kernel Debugger (KDB) public API.
 *
 * KDB is a UART-based interactive debugger for inspecting kernel state:
 * CPU registers, system registers, the process table, physical memory,
 * and arbitrary memory. It is entered automatically on kernel panic and
 * can be invoked explicitly from kernel code during development.
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
 * Disables interrupts and drops into the interactive REPL. The 'continue'
 * command returns from this function and resumes normal execution.
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

#endif /* PERSPICUA_DEBUG_KDB_H */
