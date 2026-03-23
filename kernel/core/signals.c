/*
 * signals.c - Kernel signal delivery implementation.
 *
 * This file handles the dispatching of pending signals to user processes,
 * including context saving/restoring and default action execution.
 */

#include "core/signals.h"

#include "arch/exception.h"
#include "arch/uaccess.h"

#include "sched/process.h"
#include "string.h"
#include "mm/addr.h"
/*
 * signal_handle_pending - Checks for and delivers pending signals.
 *
 * This function is called before returning to user space from an exception
 * or interrupt. It identifies the first pending signal and either performs
 * the default action or sets up the user stack for handler execution.
 */
void signal_handle_pending(struct exception_trap_frame* tf)
{
    /* Only deliver signals when returning to EL0 (user-space). */
    if ((tf->spsr_el1 & 0xF) != 0)
        return;

    struct task* curr = sched_get_current();
    if (curr && curr->skip_signals)
    {
        curr->skip_signals = 0;
        return;
    }

    int curr_pid = process_find_current();
    if (curr_pid < 0)
        return;

    struct process* curr_process = &process_table[curr_pid];

    if (curr_process->state == PROCESS_STATE_EMPTY)
        return;

    while (curr_process->pending_signals != 0)
    {
        int trailing_zeros = __builtin_ctz(curr_process->pending_signals);
        int sig = trailing_zeros + 1;

        curr_process->pending_signals &= ~(1u << trailing_zeros);

        if (trailing_zeros >= SIGNAL_COUNT)
            break;

        signal_handler_t handler = curr_process->signal_handlers[trailing_zeros];

        if (sig == SIGNAL_KILL)
        {
            process_exit(curr_pid, 128 + SIGNAL_KILL);
            sched_get_current()->state = SCHED_TASK_DEAD;
            schedule();
            return;
        }

        if (handler == SIGNAL_IGN)
            continue;

        if (handler == SIGNAL_DFL)
        {
            if (sig == SIGNAL_CHLD || sig == SIGNAL_CONT || sig == SIGNAL_USR1 || sig == SIGNAL_USR2)
                continue;

            process_exit(curr_pid, 128 + sig);
            sched_get_current()->state = SCHED_TASK_DEAD;
            schedule();
            return;
        }

#define SIGNAL_STACK_GUARD 128UL
        if (tf->sp_el0 < (sizeof(struct signal_frame) + SIGNAL_STACK_GUARD) || tf->sp_el0 >= KERNEL_VMA)
        {
            goto deliver_kill;
        }

        {
            uintptr_t new_sp = (tf->sp_el0 - sizeof(struct signal_frame)) & ~0xFUL;

            if (new_sp == 0 || new_sp >= KERNEL_VMA)
                goto deliver_kill;

            struct signal_frame frame;
            memcpy(&frame.saved_tf, tf, sizeof(struct exception_trap_frame));

            if (!validate_user_buffer((void*)new_sp, sizeof(struct signal_frame), 1)
                || copy_to_user((void*)new_sp, &frame, sizeof(struct signal_frame)) != 0)
            {
                goto deliver_kill;
            }

            tf->elr_el1 = (uintptr_t)handler;
            tf->sp_el0 = new_sp;
            tf->x[0] = (uint64_t)sig;

            if (curr_process->sig_restorer)
            {
                if (curr_process->sig_restorer >= KERNEL_VMA)
                    goto deliver_kill;
                /* x30 is the return address for the handler; it must point to the restorer */
                tf->x30 = curr_process->sig_restorer;
            }
            else
            {
                /* If no restorer, we have no safe way to return to user-space context.
                 * This usually means crt0 failed to register it. */
                goto deliver_kill;
            }
        }
        /* After setting up one signal frame, return to EL0 to run the handler.
         * Other pending signals will be handled on the next transition from EL0 to EL1. */
        return;
    }
    return;

deliver_kill:
    process_exit(curr_pid, -1);
    sched_get_current()->state = SCHED_TASK_DEAD;
    schedule();
}
