/*
 * signals.h - Kernel-internal API for signal management.
 */

#ifndef PERSPICUA_CORE_SIGNALS_H
#define PERSPICUA_CORE_SIGNALS_H

#include <stdint.h>

#include "uapi/signals.h"

#include "arch/exception.h"

// Saved on the user stack at delivery; sigreturn restores registers and mask from it.
struct signal_frame {
    struct exception_trap_frame saved_tf;
    sigset_t saved_mask;
};

// Re-issued after a signal: ERESTARTSYS under SA_RESTART, ERESTARTNOHAND when no handler ran.
#define ERESTARTSYS    512
#define ERESTARTNOHAND 514

struct process;

// p has an undelivered signal it does not block.
int signal_pending(const struct process *p);

// True when sp is inside p's alternate signal stack.
int signal_on_altstack(const struct process *p, uintptr_t sp);

void signal_handle_pending(struct exception_trap_frame *tf);

// A fault signal to the current process, overriding ignore and block; 1 if it will kill.
int signal_raise_fault(int sig);

// kill() for one process, checked and sent under one lock hold; init takes only handled signals.
int signal_kill(uint32_t sender_pid, uint32_t target_pid, int sig);

int signal_send(uint32_t target_pid, int sig);

int signal_send_group(uint32_t pgid, int sig);

#endif // PERSPICUA_CORE_SIGNALS_H
