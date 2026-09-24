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

int signal_on_altstack(const struct process *p, uintptr_t sp)
{
    uintptr_t base = (uintptr_t)p->sigaltstack.ss_sp;
    return base != 0 && sp >= base && sp < base + p->sigaltstack.ss_size;
}

// A nested handler keeps growing the alt stack it is already on.
static int altstack_usable(const struct process *p, uintptr_t sp)
{
    return p->sigaltstack.ss_sp != NULL && !(p->sigaltstack.ss_flags & SS_DISABLE)
           && !signal_on_altstack(p, sp);
}

// Headroom left below sp_el0 so a handler frame never abuts the live stack.
#define SIGNAL_STACK_GUARD 128UL

enum signal_progress {
    SIGNAL_PROGRESS_MORE,
    SIGNAL_PROGRESS_DONE,
};

/*
 * struct syscall_restart - A syscall that stopped short and can be re-issued.
 */
struct syscall_restart {
    int active;
    int nohand; // ERESTARTNOHAND: a handler turns it into EINTR whatever SA_RESTART says
    uint64_t arg0;
};

/*
 * syscall_rewind - Points the frame back at the svc that made the call.
 */
static void syscall_rewind(struct exception_trap_frame *tf, uint64_t arg0)
{
    tf->elr_el1 -= 4;
    tf->x[0] = arg0;
}

/*
 * signal_deliver_one - Pops the lowest deliverable signal and acts on it.
 */
static enum signal_progress signal_deliver_one(struct exception_trap_frame *tf, struct process *p,
                                               int pid, struct syscall_restart *restart)
{
    sigset_t deliverable = p->pending_signals & ~p->blocked_signals;
    if (deliverable == 0) {
        return SIGNAL_PROGRESS_DONE;
    }

    int bit = __builtin_ctz(deliverable);
    int sig = bit + 1;

    __atomic_fetch_and(&p->pending_signals, ~(1u << bit), __ATOMIC_SEQ_CST);

    struct sigaction *sa = &p->signal_handlers[bit];
    sighandler_t handler = sa->sa_handler;

    if (handler == SIG_IGN) {
        return SIGNAL_PROGRESS_MORE;
    }

    if (handler == SIG_DFL) {
        enum signal_default action = signal_default_action[sig];

        // A continue already happened at send time; there is nothing left to do.
        if (action == SIGNAL_DEFAULT_IGN || action == SIGNAL_DEFAULT_CONT) {
            return SIGNAL_PROGRESS_MORE;
        }

        if (action == SIGNAL_DEFAULT_STOP) {
            /*
             * Commit to the stop under process_table_lock so it is serialised
             * against a racing SIGCONT/SIGKILL in signal_send(): the sender sets
             * the pending bit and inspects our task state under the same lock.
             * If a CONT (cancels the stop) or KILL (trumps everything) slipped in
             * after we popped the stop signal above, do not park a task that no
             * one will resume -- leave it for the next pass.
             */
            const sigset_t stop_override = (1u << (SIGCONT - 1)) | (1u << (SIGKILL - 1));
            unsigned long flags = spin_lock_irqsave(&process_table_lock);
            if (p->pending_signals & stop_override) {
                spin_unlock_irqrestore(&process_table_lock, flags);
                return SIGNAL_PROGRESS_MORE;
            }
            sched_current_task()->state = SCHED_TASK_STOPPED;
            p->stop_reported = 0;
            p->stop_sig = sig;

            /* Wake a parent blocked in waitpid(WUNTRACED); without this the stop
             * is invisible and the parent sleeps until we exit instead. */
            struct process *parent = process_slot(p->parent_pid);
            if (p->parent_pid != 0 && parent && parent->state == PROCESS_STATE_RUNNING
                && parent->main_task) {
                sched_unblock(parent->main_task);
            }

            spin_unlock(&process_table_lock); // keep IRQs masked across sched_schedule()
            sched_schedule();
            irq_restore(flags);

            // Resumed. Whatever woke us is pending, so keep going.
            return SIGNAL_PROGRESS_MORE;
        }

        process_exit(pid, 128 + sig);
    }

    /* SA_ONSTACK is what makes a SIGSEGV from stack exhaustion catchable:
     * the frame needs a stack the process still has. */
    uintptr_t stack_top = tf->sp_el0;
    if ((sa->sa_flags & SA_ONSTACK) && altstack_usable(p, tf->sp_el0)) {
        stack_top = (uintptr_t)p->sigaltstack.ss_sp + p->sigaltstack.ss_size;
    }

    if (stack_top < (sizeof(struct signal_frame) + SIGNAL_STACK_GUARD) || stack_top >= KERNEL_VMA) {
        process_exit(pid, -1);
    }

    uintptr_t new_sp = (stack_top - sizeof(struct signal_frame)) & ~0xFUL;
    if (new_sp == 0 || new_sp >= KERNEL_VMA) {
        process_exit(pid, -1);
    }

    /* A handler with no way back cannot be entered: sigreturn is what restores
     * the frame below, and without a restorer the handler would return into
     * whatever x30 happened to hold. */
    if (!(sa->sa_flags & SA_RESTORER) || (uintptr_t)sa->sa_restorer == 0
        || (uintptr_t)sa->sa_restorer >= KERNEL_VMA) {
        process_exit(pid, -1);
    }

    if (restart->active) {
        if ((sa->sa_flags & SA_RESTART) && !restart->nohand) {
            syscall_rewind(tf, restart->arg0);
        } else {
            tf->x[0] = (uint64_t)-EINTR;
        }
        restart->active = 0;
    }

    // new_mask builds on the current mask; the frame carries the one
    // sigsuspend displaced, which is what sigreturn restores.
    sigset_t old_mask = p->blocked_signals;
    sigset_t frame_mask = p->has_saved_sigmask ? p->saved_sigmask : old_mask;
    sigset_t new_mask = old_mask | sa->sa_mask;
    if (!(sa->sa_flags & SA_NODEFER)) {
        new_mask |= (1u << bit);
    }
    new_mask &= ~((1u << (SIGKILL - 1)) | (1u << (SIGSTOP - 1)));
    p->blocked_signals = new_mask;

    // Zeroed first: the struct's trailing padding goes to the user stack too.
    struct signal_frame frame;
    memset(&frame, 0, sizeof(frame));
    memcpy(&frame.saved_tf, tf, sizeof(struct exception_trap_frame));
    frame.saved_mask = frame_mask;

    if (!syscall_validate_user_buffer((void *)new_sp, sizeof(struct signal_frame), 1)
        || copy_to_user((void *)new_sp, &frame, sizeof(struct signal_frame)) != 0) {
        p->blocked_signals = old_mask;
        process_exit(pid, -1);
    }

    // The frame owns it now; signal_handle_pending must not restore it again.
    p->has_saved_sigmask = 0;

    // Redirect execution to user-space handler
    tf->elr_el1 = (uintptr_t)handler;
    tf->sp_el0 = new_sp;
    tf->x[0] = (uint64_t)sig;
    tf->x30 = (uintptr_t)sa->sa_restorer;

    if (sa->sa_flags & SA_RESETHAND) {
        sa->sa_handler = SIG_DFL;
    }

    return SIGNAL_PROGRESS_DONE;
}

static void signal_deliver_all(struct exception_trap_frame *tf, struct task *curr,
                               struct syscall_restart *restart)
{
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

    while (signal_deliver_one(tf, curr_process, curr_pid, restart) == SIGNAL_PROGRESS_MORE) {
        ;
    }
}

/*
 * signal_handle_pending - Dispatches signals before returning to user mode.
 */
void signal_handle_pending(struct exception_trap_frame *tf)
{
    if ((tf->spsr_el1 & 0xF) != 0) {
        return;
    }

    struct task *curr = sched_current_task();
    struct syscall_restart restart = {0};

    if (curr) {
        uint64_t ret = tf->x[0];
        restart.nohand = ret == (uint64_t)-ERESTARTNOHAND;
        restart.active = curr->in_syscall && (ret == (uint64_t)-ERESTARTSYS || restart.nohand);
        restart.arg0 = curr->syscall_arg0;
        curr->in_syscall = 0;
    }

    signal_deliver_all(tf, curr, &restart);

    struct process *p = process_current();
    if (p && p->has_saved_sigmask) {
        p->blocked_signals = p->saved_sigmask;
        p->has_saved_sigmask = 0;
    }

    if (restart.active) {
        syscall_rewind(tf, restart.arg0);
    } else if (curr) {
        curr->sleep_resume_at = 0;
    }
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

int signal_raise_fault(int sig)
{
    struct process *p = process_current();
    if (!p || sig < 1 || sig >= NSIG) {
        return 1;
    }

    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    struct sigaction *sa = &p->signal_handlers[sig - 1];

    /* The faulting instruction re-runs on return, so ignoring or blocking a
     * synchronous fault would spin at EL0 forever. POSIX leaves that
     * undefined; forcing the default is what makes it terminate. */
    if (sa->sa_handler == SIG_IGN || (p->blocked_signals & (1u << (sig - 1)))) {
        memset(sa, 0, sizeof(*sa));
        sa->sa_handler = SIG_DFL;
        p->blocked_signals &= ~(1u << (sig - 1));
    }

    int fatal = (sa->sa_handler == SIG_DFL);
    __atomic_fetch_or(&p->pending_signals, (1u << (sig - 1)), __ATOMIC_SEQ_CST);
    spin_unlock_irqrestore(&process_table_lock, flags);

    return fatal;
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
