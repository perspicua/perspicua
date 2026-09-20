/*
 * signals.h - Kernel-internal API for signal management.
 */

#ifndef PERSPICUA_CORE_SIGNALS_H
#define PERSPICUA_CORE_SIGNALS_H

#include "uapi/signals.h"

#include <stdint.h>

#include "arch/exception.h"

/*
 * struct signal_frame - State saved on the user stack during signal delivery.
 *
 * This allows the kernel to restore the process state (including the
 * register set and signal mask) once the signal handler returns.
 */
struct signal_frame {
    struct exception_trap_frame saved_tf;
    sigset_t saved_mask;
};

#define ERESTARTSYS 512

struct process;

/*
 * signal_pending - True when p has an undelivered signal it is not blocking.
 */
int signal_pending(const struct process *p);

// True when sp is inside p's alternate signal stack.
int signal_on_altstack(const struct process *p, uintptr_t sp);

void signal_handle_pending(struct exception_trap_frame *tf);

/*
 * Posts a synchronous fault signal to the current process, overriding an
 * ignore or a block. Returns 1 if it will terminate the process.
 */
int signal_raise_fault(int sig);

int signal_send(uint32_t target_pid, int sig);

int signal_send_group(uint32_t pgid, int sig);

#endif // PERSPICUA_CORE_SIGNALS_H
