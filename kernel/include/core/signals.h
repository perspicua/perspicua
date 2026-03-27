/*
 * signals.h - Kernel-internal API for signal management.
 *
 * Handles the lifecycle of signal delivery, including context saving
 * and stack frame preparation.
 */

#ifndef PERSPICUA_CORE_SIGNALS_H
#define PERSPICUA_CORE_SIGNALS_H

#include "uapi/signals.h"

#include "arch/exception.h"

/* --- Data Structures --- */

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

/* --- Function Prototypes --- */

/*
 * signal_handle_pending - Dispatches pending signals before returning to user mode.
 */
void signal_handle_pending(struct exception_trap_frame *tf);

/*
 * signal_send - Posts a signal to a target process by its PID.
 */
int signal_send(uint32_t target_pid, int sig);

#endif /* PERSPICUA_CORE_SIGNALS_H */
