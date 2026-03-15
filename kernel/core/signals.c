/*
 * signals.c - Kernel signal delivery implementation.
 *
 * This file handles the dispatching of pending signals to user processes,
 * including context saving/restoring and default action execution.
 */

#include "signals.h"

#include "arch/exception.h"
#include "arch/uaccess.h"

#include "process.h"
#include "string.h"

/*
 * signal_handle_pending - Checks for and delivers pending signals.
 *
 * This function is called before returning to user space from an exception
 * or interrupt. It identifies the first pending signal and either performs
 * the default action or sets up the user stack for handler execution.
 */
void signal_handle_pending(struct exception_trap_frame* tf)
{
    if ((tf->spsr_el1 & 0xF) != 0)
        return;

    int curr_pid = process_find_current();
    if (curr_pid < 0)
        return;

    struct process* curr_process = &process_table[curr_pid];
    if (curr_process->state == PROCESS_STATE_EMPTY)
        return;

    if (curr_process->pending_signals == 0)
        return;

    int trailing_zeros       = __builtin_ctz(curr_process->pending_signals);
    int sig                  = trailing_zeros + 1;
    signal_handler_t handler = curr_process->signal_handlers[trailing_zeros];

    if (sig == SIGNAL_KILL)
    {
        process_exit(curr_pid, 128 + SIGNAL_KILL);
        return;
    }

    if (handler == SIGNAL_IGN)
    {
        curr_process->pending_signals &= ~(1 << (trailing_zeros));
        return;
    }
    else if (handler == SIGNAL_DFL)
    {
        if (sig == SIGNAL_CHLD || sig == SIGNAL_CONT || sig == SIGNAL_USR1 || sig == SIGNAL_USR2)
        {
            curr_process->pending_signals &= ~(1 << (trailing_zeros));
            return;
        }

        process_exit(curr_pid, 128 + sig);
        return;
    }
    else
    {
        uintptr_t new_sp = (tf->sp_el0 - sizeof(struct signal_frame));
        new_sp &= ~0xF;

        struct signal_frame frame;
        memcpy(&frame.saved_tf, tf, sizeof(struct exception_trap_frame));

        if (copy_to_user((void*)new_sp, &frame, sizeof(struct signal_frame)) != 0)
        {
            process_exit(curr_pid, -1);
            return;
        }

        tf->elr_el1 = (uintptr_t)handler;
        tf->sp_el0  = new_sp;
        tf->x[0]    = sig;

        if (curr_process->sig_restorer)
        {
            tf->x30 = curr_process->sig_restorer;
        }

        curr_process->pending_signals &= ~(1 << (trailing_zeros));
    }
}
