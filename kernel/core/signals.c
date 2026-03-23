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

#include "stdio.h"
#include "uapi/syscalls.h"
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

    /* Find a pending signal that is NOT blocked */
    sigset_t deliverable = curr_process->pending_signals & ~curr_process->blocked_signals;
    if (deliverable == 0)
        return;

    int trailing_zeros = __builtin_ctz(deliverable);
    int sig = trailing_zeros + 1;

    /* Pop the signal */
    __atomic_fetch_and(&curr_process->pending_signals, ~(1u << trailing_zeros), __ATOMIC_SEQ_CST);

    struct sigaction* sa = &curr_process->signal_handlers[trailing_zeros];
    signal_handler_t handler = sa->sa_handler;

    if (handler == SIGNAL_IGN)
    {
        return;
    }

    if (handler == SIGNAL_DFL)
    {
        /* Default actions */
        if (sig == SIGNAL_CHLD || sig == SIGNAL_CONT || sig == SIGNAL_USR1 || sig == SIGNAL_USR2 || sig == SIGNAL_WINCH
            || sig == SIGNAL_URG)
        {
            return;
        }

        if (sig == SIGNAL_STOP || sig == SIGNAL_TSTP || sig == SIGNAL_TTIN || sig == SIGNAL_TTOU)
        {
            /* For now, just ignore stop signals as we don't have job control yet */
            return;
        }

        process_exit(curr_pid, 128 + sig);
        sched_get_current()->state = SCHED_TASK_DEAD;
        schedule();
        return;
    }

    /* Deliver signal to user handler */
#define SIGNAL_STACK_GUARD 128UL
    if (tf->sp_el0 < (sizeof(struct signal_frame) + SIGNAL_STACK_GUARD) || tf->sp_el0 >= KERNEL_VMA)
    {
        goto deliver_kill;
    }

    {
        uintptr_t new_sp = (tf->sp_el0 - sizeof(struct signal_frame)) & ~0xFUL;

        if (new_sp == 0 || new_sp >= KERNEL_VMA)
            goto deliver_kill;

        /* Update process signal mask BEFORE potential blocking operations or returning */
        sigset_t old_mask = curr_process->blocked_signals;
        sigset_t new_mask = old_mask | sa->sa_mask;
        if (!(sa->sa_flags & SA_NODEFER))
        {
            new_mask |= (1u << (sig - 1));
        }
        /* Ensure KILL and STOP cannot be blocked */
        new_mask &= ~((1u << (SIGNAL_KILL - 1)) | (1u << (SIGNAL_STOP - 1)));
        curr_process->blocked_signals = new_mask;

        struct signal_frame frame;
        memcpy(&frame.saved_tf, tf, sizeof(struct exception_trap_frame));
        frame.saved_mask = old_mask;

        if (!validate_user_buffer((void*)new_sp, sizeof(struct signal_frame), 1)
            || copy_to_user((void*)new_sp, &frame, sizeof(struct signal_frame)) != 0)
        {
            /* Rollback mask on failure */
            curr_process->blocked_signals = old_mask;
            goto deliver_kill;
        }

        /* Prepare trap frame for handler execution */
        tf->elr_el1 = (uintptr_t)handler;
        tf->sp_el0 = new_sp;
        tf->x[0] = (uint64_t)sig;

        if (sa->sa_flags & SA_RESTORER)
        {
            if ((uintptr_t)sa->sa_restorer >= KERNEL_VMA)
                goto deliver_kill;
            tf->x30 = (uintptr_t)sa->sa_restorer;
        }
        else
        {
            if (curr_process->default_sigrestorer == 0 || curr_process->default_sigrestorer >= KERNEL_VMA)
                goto deliver_kill;
            tf->x30 = curr_process->default_sigrestorer;
        }

        if (sa->sa_flags & SA_RESETHAND)
        {
            sa->sa_handler = SIGNAL_DFL;
        }
    }
    return;

deliver_kill:
    process_exit(curr_pid, -1);
    sched_get_current()->state = SCHED_TASK_DEAD;
    schedule();
}
