/*
 * signals.c - Core implementation of the signal handling subsystem.
 */

#include "core/signals.h"

#include <stddef.h>
#include <stdint.h>

#include "stdio.h"
#include "string.h"

#include "uapi/errno.h"
#include "uapi/syscalls.h"

#include "arch/exception.h"
#include "arch/uaccess.h"

#include "mm/addr.h"
#include "sched/sched.h"
#include "sched/process.h"
#include "core/lock.h"
#include "arch/irq.h"

enum signal_default {
    SIGNAL_DEFAULT_TERM,
    SIGNAL_DEFAULT_IGN,
    SIGNAL_DEFAULT_STOP,
    SIGNAL_DEFAULT_CONT,
};

static const enum signal_default signal_default_action[NSIG] = {
    [SIGHUP] = SIGNAL_DEFAULT_TERM,  [SIGINT] = SIGNAL_DEFAULT_TERM,
    [SIGQUIT] = SIGNAL_DEFAULT_TERM, [SIGILL] = SIGNAL_DEFAULT_TERM,
    [SIGTRAP] = SIGNAL_DEFAULT_TERM, [SIGABRT] = SIGNAL_DEFAULT_TERM,
    [SIGBUS] = SIGNAL_DEFAULT_TERM,  [SIGFPE] = SIGNAL_DEFAULT_TERM,
    [SIGKILL] = SIGNAL_DEFAULT_TERM, [SIGUSR1] = SIGNAL_DEFAULT_TERM,
    [SIGSEGV] = SIGNAL_DEFAULT_TERM, [SIGUSR2] = SIGNAL_DEFAULT_TERM,
    [SIGPIPE] = SIGNAL_DEFAULT_TERM, [SIGALRM] = SIGNAL_DEFAULT_TERM,
    [SIGTERM] = SIGNAL_DEFAULT_TERM, [SIGSTKFLT] = SIGNAL_DEFAULT_TERM,
    [SIGCHLD] = SIGNAL_DEFAULT_IGN,  [SIGCONT] = SIGNAL_DEFAULT_CONT,
    [SIGSTOP] = SIGNAL_DEFAULT_STOP, [SIGTSTP] = SIGNAL_DEFAULT_STOP,
    [SIGTTIN] = SIGNAL_DEFAULT_STOP, [SIGTTOU] = SIGNAL_DEFAULT_STOP,
    [SIGURG] = SIGNAL_DEFAULT_IGN,   [SIGXCPU] = SIGNAL_DEFAULT_TERM,
    [SIGXFSZ] = SIGNAL_DEFAULT_TERM, [SIGVTALRM] = SIGNAL_DEFAULT_TERM,
    [SIGPROF] = SIGNAL_DEFAULT_TERM, [SIGWINCH] = SIGNAL_DEFAULT_IGN,
    [SIGIO] = SIGNAL_DEFAULT_TERM,   [SIGPWR] = SIGNAL_DEFAULT_TERM,
    [SIGSYS] = SIGNAL_DEFAULT_TERM,
};

/*
 * signal_discarded - True when posting sig to p would have no effect a
 * handler or a default action could observe, so no pending bit is set.
 */
static int signal_discarded(const struct process *p, int sig)
{
    // Neither disposition can be changed, so neither can be dropped.
    if (sig == SIGKILL || sig == SIGSTOP) {
        return 0;
    }

    sighandler_t handler = p->signal_handlers[sig - 1].sa_handler;
    if (handler == SIG_IGN) {
        return 1;
    }
    return handler == SIG_DFL && signal_default_action[sig] == SIGNAL_DEFAULT_IGN;
}

int signal_pending(const struct process *p)
{
    return p && (p->pending_signals & ~p->blocked_signals) != 0;
}

// Headroom left below sp_el0 so a handler frame never abuts the live stack.
#define SIGNAL_STACK_GUARD 128UL

/*
 * signal_handle_pending - Dispatches signals before returning to user mode.
 */
void signal_handle_pending(struct exception_trap_frame *tf)
{
    // Signals are only deliverable when returning to user-space (EL0)
    if ((tf->spsr_el1 & 0xF) != 0) {
        return;
    }

    struct task *curr = sched_current_task();
    if (curr && curr->skip_signals) {
        curr->skip_signals = 0;
        return;
    }

    int curr_pid = process_current_pid();
    if (curr_pid < 0) {
        return;
    }

    struct process *curr_process = process_slot((uint32_t)curr_pid);
    if (!curr_process) {
        return;
    }

    sigset_t deliverable = curr_process->pending_signals & ~curr_process->blocked_signals;
    if (deliverable == 0) {
        return;
    }

    int bit = __builtin_ctz(deliverable);
    int sig = bit + 1;

    __atomic_fetch_and(&curr_process->pending_signals, ~(1u << bit), __ATOMIC_SEQ_CST);

    struct sigaction *sa = &curr_process->signal_handlers[bit];
    sighandler_t handler = sa->sa_handler;

    if (handler == SIG_IGN) {
        return;
    }

    if (handler == SIG_DFL) {
        enum signal_default action = signal_default_action[sig];

        // A continue already happened at send time; there is nothing left to do.
        if (action == SIGNAL_DEFAULT_IGN || action == SIGNAL_DEFAULT_CONT) {
            return;
        }

        if (action == SIGNAL_DEFAULT_STOP) {
            /*
             * Commit to the stop under process_table_lock so it is serialised
             * against a racing SIGCONT/SIGKILL in signal_send(): the sender sets
             * the pending bit and inspects our task state under the same lock.
             * If a CONT (cancels the stop) or KILL (trumps everything) slipped in
             * after we popped the stop signal above, do not park a task that no
             * one will resume -- return so the pending signal is delivered next.
             */
            const sigset_t stop_override = (1u << (SIGCONT - 1)) | (1u << (SIGKILL - 1));
            unsigned long flags = spin_lock_irqsave(&process_table_lock);
            if (curr_process->pending_signals & stop_override) {
                spin_unlock_irqrestore(&process_table_lock, flags);
                return;
            }
            sched_current_task()->state = SCHED_TASK_STOPPED;
            curr_process->stop_reported = 0;

            /* Wake a parent blocked in waitpid(WUNTRACED); without this the stop
             * is invisible and the parent sleeps until we exit instead. */
            struct process *parent = process_slot(curr_process->parent_pid);
            if (curr_process->parent_pid != 0 && parent && parent->state == PROCESS_STATE_RUNNING
                && parent->main_task) {
                sched_unblock(parent->main_task);
            }

            spin_unlock(&process_table_lock); // keep IRQs masked across sched_schedule()
            sched_schedule();
            irq_restore(flags);
            return;
        }

        process_exit(curr_pid, 128 + sig);
    }

    // Verify user stack has enough space for the signal frame
    if (tf->sp_el0 < (sizeof(struct signal_frame) + SIGNAL_STACK_GUARD)
        || tf->sp_el0 >= KERNEL_VMA) {
        goto deliver_kill;
    }

    {
        uintptr_t new_sp = (tf->sp_el0 - sizeof(struct signal_frame)) & ~0xFUL;
        if (new_sp == 0 || new_sp >= KERNEL_VMA) {
            goto deliver_kill;
        }

        // Update process mask; ensure KILL/STOP remain unblockable
        sigset_t old_mask = curr_process->blocked_signals;
        sigset_t new_mask = old_mask | sa->sa_mask;
        if (!(sa->sa_flags & SA_NODEFER)) {
            new_mask |= (1u << bit);
        }
        new_mask &= ~((1u << (SIGKILL - 1)) | (1u << (SIGSTOP - 1)));
        curr_process->blocked_signals = new_mask;

        struct signal_frame frame;
        memcpy(&frame.saved_tf, tf, sizeof(struct exception_trap_frame));
        frame.saved_mask = old_mask;

        if (!syscall_validate_user_buffer((void *)new_sp, sizeof(struct signal_frame), 1)
            || copy_to_user((void *)new_sp, &frame, sizeof(struct signal_frame)) != 0) {
            curr_process->blocked_signals = old_mask;
            goto deliver_kill;
        }

        // Redirect execution to user-space handler
        tf->elr_el1 = (uintptr_t)handler;
        tf->sp_el0 = new_sp;
        tf->x[0] = (uint64_t)sig;

        if (!(sa->sa_flags & SA_RESTORER) || (uintptr_t)sa->sa_restorer == 0
            || (uintptr_t)sa->sa_restorer >= KERNEL_VMA) {
            goto deliver_kill;
        }
        tf->x30 = (uintptr_t)sa->sa_restorer;

        if (sa->sa_flags & SA_RESETHAND) {
            sa->sa_handler = SIG_DFL;
        }
    }
    return;

deliver_kill:
    process_exit(curr_pid, -1);
}

/*
 * signal_send_target_locked - Posts a signal to a process.
 *
 * Precondition: process_table_lock MUST be held by caller.
 * Checks that the process is non-NULL and in state PROCESS_STATE_RUNNING
 * before dereferencing main_task or modifying pending_signals.
 *
 * Returns 0 (0) on success, or -ESRCH if p is invalid/not running.
 */
static int signal_send_target_locked(struct process *p, int sig)
{
    if (!p || p->state != PROCESS_STATE_RUNNING) {
        return -ESRCH;
    }

    const sigset_t stop_mask = (1u << (SIGSTOP - 1)) | (1u << (SIGTSTP - 1)) | (1u << (SIGTTIN - 1))
                               | (1u << (SIGTTOU - 1));

    if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) {
        __atomic_fetch_and(&p->pending_signals, ~(1u << (SIGCONT - 1)), __ATOMIC_SEQ_CST);
    } else if (sig == SIGCONT) {
        __atomic_fetch_and(&p->pending_signals, ~stop_mask, __ATOMIC_SEQ_CST);
    }

    int discarded = signal_discarded(p, sig);
    if (!discarded) {
        __atomic_fetch_or(&p->pending_signals, (1u << (sig - 1)), __ATOMIC_SEQ_CST);
    }

    /* Resuming a stopped task is an effect of SENDING SIGCONT, not of
     * delivering it, so it happens even when the disposition is ignore. */
    if ((sig == SIGCONT || sig == SIGKILL) && p->main_task
        && p->main_task->state == SCHED_TASK_STOPPED) {
        p->stop_reported = 0;
        sched_continue(p->main_task);
    }

    if (!discarded && p->main_task && p->main_task->state == SCHED_TASK_BLOCKED) {
        sched_unblock(p->main_task);
    }

    return 0;
}

int signal_send(uint32_t target_pid, int sig)
{
    if (sig < 1 || sig >= NSIG) {
        return -EINVAL;
    }

    if (target_pid == 0 || target_pid >= PROCESS_TABLE_SIZE) {
        return -ESRCH;
    }

    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    int res = signal_send_target_locked(process_table[target_pid], sig);
    spin_unlock_irqrestore(&process_table_lock, flags);
    return res;
}

/*
 * signal_send_group - Sends a signal to all processes in a process group.
 *
 * Walks process_table under process_table_lock (O(PROCESS_TABLE_SIZE)).
 * Returns 0 if delivered to at least one process,
 * or -ESRCH if no matching running process was found.
 */
int signal_send_group(uint32_t pgid, int sig)
{
    if (sig < 1 || sig >= NSIG) {
        return -EINVAL;
    }

    if (pgid == 0) {
        return -ESRCH;
    }

    int targets_reached = 0;

    unsigned long flags = spin_lock_irqsave(&process_table_lock);

    // O(PROCESS_TABLE_SIZE) walk under lock over all process slots
    for (uint32_t i = 1; i < PROCESS_TABLE_SIZE; i++) {
        struct process *p = process_table[i];
        if (p && p->state == PROCESS_STATE_RUNNING && p->pgid == pgid) {
            if (signal_send_target_locked(p, sig) == 0) {
                targets_reached++;
            }
        }
    }

    spin_unlock_irqrestore(&process_table_lock, flags);

    return (targets_reached > 0) ? 0 : -ESRCH;
}
