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
};

/* --- Signal Dispatch & Delivery --- */

/* Processes pending signals for the current process before returning to user mode */
void signal_handle_pending(struct exception_trap_frame* tf);

#endif /* PERSPICUA_KERNEL_SIGNALS_H */
