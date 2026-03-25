/*
 * signals.h - Kernel-internal API for signals.
 *
 * This file provides internal kernel structures and functions for signal
 * delivery and handling within the scheduler and exception layers.
 */

#ifndef PERSPICUA_KERNEL_SIGNALS_H
#define PERSPICUA_KERNEL_SIGNALS_H

#include "uapi/signals.h"

#include "arch/exception.h"

/* Structure saved on the user stack during signal delivery */
struct signal_frame
{
    struct exception_trap_frame saved_tf;
    sigset_t saved_mask;
};

/* --- Signal Dispatch & Delivery --- */

/* Processes pending signals for the current process before returning to user mode */
void signal_handle_pending(struct exception_trap_frame* tf);

/* Sends a signal to a process by its PID. Returns 0 on success, or a negative error. */
int signal_send(uint32_t target_pid, int sig);

#endif /* PERSPICUA_KERNEL_SIGNALS_H */
