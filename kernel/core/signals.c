/*
 * signals.c - Kernel signal delivery implementation.
 *
 * This module manages signal dispatching, including handler setup
 * and context restoration during user-space returns.
 */

#include "core/signals.h"

#include "stdio.h"
#include "string.h"

#include "uapi/errors.h"
#include "uapi/syscalls.h"

#include "arch/exception.h"
#include "arch/uaccess.h"

#include "mm/addr.h"
#include "sched/sched.h"
#include "sched/process.h"

/*
 * signal_handle_pending - Dispatches signals before returning to user mode.
 *
 * Called from the exception return path. Sets up the user stack with a signal
 * frame if a handler is registered, or performs default actions.
 */
void signal_handle_pending(struct exception_trap_frame *tf)
{
    /* Signals are only deliverable when returning to user-space (EL0) */
    if ((tf->spsr_el1 & 0xF) != 0) {
        return;
    }

    struct task *curr = sched_get_current();
    if (curr && curr->skip_signals) {
        curr->skip_signals = 0;
        return;
    }

    int curr_pid = process_find_current();
    if (curr_pid < 0) {
        return;
    }

    struct process *curr_process = &process_table[curr_pid];
    if (curr_process->state == PROCESS_STATE_EMPTY) {
        return;
    }

    /* Identify first unblocked pending signal */
    sigset_t deliverable = curr_process->pending_signals & ~curr_process->blocked_signals;
    if (deliverable == 0) {
        return;
    }

    int trailing_zeros = __builtin_ctz(deliverable);
    int sig = trailing_zeros + 1;

    /* Pop signal from pending set atomically */
    __atomic_fetch_and(&curr_process->pending_signals, ~(1u << trailing_zeros), __ATOMIC_SEQ_CST);

    struct sigaction *sa = &curr_process->signal_handlers[trailing_zeros];
    signal_handler_t handler = sa->sa_handler;

    if (handler == SIGNAL_IGN) {
        return;
    }

    if (handler == SIGNAL_DFL) {
        /* Signals with default 'ignore' actions */
        if (sig == SIGNAL_CHLD || sig == SIGNAL_CONT || sig == SIGNAL_USR1 || sig == SIGNAL_USR2
            || sig == SIGNAL_WINCH || sig == SIGNAL_URG) {
            return;
        }

        /* Default 'stop' actions (ignored until job control is implemented) */
        if (sig == SIGNAL_STOP || sig == SIGNAL_TSTP || sig == SIGNAL_TTIN || sig == SIGNAL_TTOU) {
            return;
        }

        /* Terminate process for all other signals */
        process_exit(curr_pid, 128 + sig);
        sched_get_current()->state = SCHED_TASK_DEAD;
        schedule();
        return;
    }

/* Stack alignment and safety guard */
#define SIGNAL_STACK_GUARD 128UL

    /* Verify user stack has enough space for the signal frame */
    if (tf->sp_el0 < (sizeof(struct signal_frame) + SIGNAL_STACK_GUARD)
        || tf->sp_el0 >= KERNEL_VMA) {
        goto deliver_kill;
    }

    {
        uintptr_t new_sp = (tf->sp_el0 - sizeof(struct signal_frame)) & ~0xFUL;
        if (new_sp == 0 || new_sp >= KERNEL_VMA) {
            goto deliver_kill;
        }

        /* Update process mask; ensure KILL/STOP remain unblockable */
        sigset_t old_mask = curr_process->blocked_signals;
        sigset_t new_mask = old_mask | sa->sa_mask;
        if (!(sa->sa_flags & SA_NODEFER)) {
            new_mask |= (1u << (sig - 1));
        }
        new_mask &= ~((1u << (SIGNAL_KILL - 1)) | (1u << (SIGNAL_STOP - 1)));
        curr_process->blocked_signals = new_mask;

        struct signal_frame frame;
        memcpy(&frame.saved_tf, tf, sizeof(struct exception_trap_frame));
        frame.saved_mask = old_mask;

        if (!validate_user_buffer((void *)new_sp, sizeof(struct signal_frame), 1)
            || copy_to_user((void *)new_sp, &frame, sizeof(struct signal_frame)) != 0) {
            curr_process->blocked_signals = old_mask;
            goto deliver_kill;
        }

        /* Redirect execution to user-space handler */
        tf->elr_el1 = (uintptr_t)handler;
        tf->sp_el0 = new_sp;
        tf->x[0] = (uint64_t)sig;

        if (sa->sa_flags & SA_RESTORER) {
            if ((uintptr_t)sa->sa_restorer >= KERNEL_VMA) {
                goto deliver_kill;
            }
            tf->x30 = (uintptr_t)sa->sa_restorer;
        } else {
            if (curr_process->default_sigrestorer == 0
                || curr_process->default_sigrestorer >= KERNEL_VMA) {
                goto deliver_kill;
            }
            tf->x30 = curr_process->default_sigrestorer;
        }

        if (sa->sa_flags & SA_RESETHAND) {
            sa->sa_handler = SIGNAL_DFL;
        }
    }
    return;

deliver_kill:
    process_exit(curr_pid, -1);
    sched_get_current()->state = SCHED_TASK_DEAD;
    schedule();
}

/*
 * signal_send - Posts a signal to a process and wakes it if blocked.
 */
int signal_send(uint32_t target_pid, int sig)
{
    if (sig < 1 || sig >= SIGNAL_COUNT) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    if (target_pid == 0 || target_pid >= PROCESS_TABLE_SIZE
        || process_table[target_pid].state == PROCESS_STATE_EMPTY) {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }

    struct process *p = &process_table[target_pid];

    __atomic_fetch_or(&p->pending_signals, (1u << (sig - 1)), __ATOMIC_SEQ_CST);

    if (p->signal_handlers[sig - 1].sa_handler != SIGNAL_IGN) {
        if (p->main_task && p->main_task->state == SCHED_TASK_BLOCKED) {
            sched_unblock(p->main_task);
        }
    }

    return PERS_SUCCESS;
}
