/*
 * syscall.c - Implementation of the system call dispatcher.
 *
 * This module validates user-mode memory access and dispatches
 * system calls to their respective kernel implementations.
 */

#include "core/syscall.h"

#include "stdio.h"
#include "string.h"
#include "panic.h"

#include "uapi/mman.h"
#include "uapi/syscalls.h"
#include "uapi/errors.h"

#include "arch/uaccess.h"

#include "core/signals.h"
#include "core/tty.h"
#include "core/timer.h"
#include "fs/vfs.h"
#include "fs/devfs.h"
#include "fs/pipe.h"
#include "mm/pmm.h"
#include "mm/mmu.h"
#include "mm/addr.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "sched/process.h"
#include "driver/uart.h"
#include "driver/block.h"
#include "fs/pagecache.h"

/*
 * validate_user_buffer - Verifies that a memory range is valid for user access.
 */
int validate_user_buffer(const void *ptr, size_t len, int writable)
{
    if (!ptr || len == 0) {
        return 0;
    }

    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end = start + len;

    /* Prevent wrap-around or kernel-space intrusion */
    if (end < start || end > KERNEL_VMA) {
        return 0;
    }

    /*
     * These are the calling process's own tables, and it cannot be exiting
     * while it is here, so the lock is only needed to read the pointer -- not
     * for the walk. Holding it across a megabyte-scale range stalls every other
     * core, including any that needs it to schedule.
     */
    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    struct process *p = process_current();
    unsigned long *pgd = p ? p->user_pgd : NULL;
    spin_unlock_irqrestore(&process_table_lock, flags);

    if (!pgd) {
        return 0;
    }

    return mmu_user_range_ok(pgd, start, end, writable);
}

/*
 * pgid_session_locked - The session a process group belongs to, or 0 if the
 * group has no members. A group is only a number its members share, so an empty
 * one means "no such group" rather than "session 0".
 *
 * Precondition: process_table_lock MUST be held by the caller.
 */
static uint32_t pgid_session_locked(uint32_t pgid)
{
    if (pgid == 0) {
        return 0;
    }

    for (uint32_t i = 1; i < PROCESS_TABLE_SIZE; i++) {
        struct process *p = process_table[i];
        if (p && p->state == PROCESS_STATE_RUNNING && p->pgid == pgid) {
            return p->sid;
        }
    }
    return 0;
}

/*
 * copy_path_from_user - Copies a user path into a fresh kernel buffer.
 *
 * On success the caller owns *out and must heap_free it. Every path-taking
 * syscall goes through here so the allocation and truncation checks cannot be
 * forgotten at one call site.
 */
static int copy_path_from_user(const char *upath, char **out)
{
    *out = NULL;

    char *kpath = heap_malloc(VFS_MAX_PATH_LEN);
    if (!kpath) {
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    long copied = strncpy_from_user(kpath, upath, VFS_MAX_PATH_LEN);
    if (copied < 0) {
        heap_free(kpath);
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    *out = kpath;
    return PERS_SUCCESS;
}

/*
 * syscall_handle - The primary entry point for EL0 synchronous exceptions.
 */
void syscall_handle(struct exception_trap_frame *tf)
{
    uint64_t syscall_nr = tf->x[8];
    struct task *curr = sched_get_current();
    uint32_t pid = curr->pid;

    /* A task running a syscall always has a live slot; refuse rather than
     * fault at EL1 if that ever stops holding. */
    struct process *proc = process_slot(pid);
    if (!proc) {
        tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
        return;
    }

    switch (syscall_nr) {
        case SYS_WRITE: {
            int fd = (int)(tf->x[0]);
            const char *buf = (const char *)(tf->x[1]);
            size_t len = (size_t)(tf->x[2]);

            /* Enforce maximum RW size to prevent excessive heap usage */
            if (len == 0 || len > SYSCALL_MAX_RW_SIZE) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            if (!validate_user_buffer(buf, len, 0)) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            char *kbuf = heap_malloc(len);
            if (!kbuf) {
                tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }

            if (copy_from_user(kbuf, buf, len) != 0) {
                heap_free(kbuf);
                tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }

            int bytes = vfs_write(fd, kbuf, len);
            heap_free(kbuf);
            tf->x[0] = (uint64_t)bytes;
            break;
        }

        case SYS_PWRITE: {
            int fd = (int)(tf->x[0]);
            const char *buf = (const char *)(tf->x[1]);
            size_t len = (size_t)(tf->x[2]);
            vfs_off_t offset = (vfs_off_t)(tf->x[3]);

            /* Enforce maximum RW size to prevent excessive heap usage */
            if (len == 0 || len > SYSCALL_MAX_RW_SIZE) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            if (!validate_user_buffer(buf, len, 0)) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            char *kbuf = heap_malloc(len);
            if (!kbuf) {
                tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }

            if (copy_from_user(kbuf, buf, len) != 0) {
                heap_free(kbuf);
                tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }

            int bytes = vfs_pwrite(fd, kbuf, len, offset);
            heap_free(kbuf);
            tf->x[0] = (uint64_t)bytes;
            break;
        }

        case SYS_EXIT: {
            int status = (int)tf->x[0];
            process_exit(pid, status);
            curr->state = SCHED_TASK_DEAD;
            schedule();
            break;
        }

        case SYS_GETPID: {
            tf->x[0] = (uint64_t)pid;
            break;
        }

        case SYS_GETPPID: {
            tf->x[0] = (uint64_t)proc->parent_pid;
            break;
        }

        case SYS_YIELD: {
            schedule();
            tf->x[0] = PERS_SUCCESS;
            break;
        }

        case SYS_SLEEP: {
            unsigned long ms = tf->x[0];
            sched_sleep_ms(ms);
            tf->x[0] = PERS_SUCCESS;
            break;
        }

        case SYS_OPEN: {
            const char *path = (const char *)(tf->x[0]);
            int flags = (int)(tf->x[1]);

            char *kpath;
            int err = copy_path_from_user(path, &kpath);
            if (err != PERS_SUCCESS) {
                tf->x[0] = (uint64_t)err;
                break;
            }

            int fd = vfs_open(kpath, flags);
            heap_free(kpath);
            tf->x[0] = (uint64_t)fd;
            break;
        }

        case SYS_READ: {
            int fd = (int)(tf->x[0]);
            char *buf = (char *)(tf->x[1]);
            size_t len = (size_t)(tf->x[2]);

            if (len == 0 || len > SYSCALL_MAX_RW_SIZE) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            if (!validate_user_buffer(buf, len, 1)) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            char *kbuf = heap_malloc(len);
            if (!kbuf) {
                tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }

            int bytes = vfs_read(fd, kbuf, len);
            if (bytes > 0) {
                if (copy_to_user(buf, kbuf, (size_t)bytes) != 0) {
                    bytes = -PERS_ERR_OUT_OF_MEMORY;
                }
            }
            heap_free(kbuf);
            tf->x[0] = (uint64_t)bytes;
            break;
        }

        case SYS_PREAD: {
            int fd = (int)(tf->x[0]);
            char *buf = (char *)(tf->x[1]);
            size_t len = (size_t)(tf->x[2]);
            vfs_off_t offset = (vfs_off_t)(tf->x[3]);

            if (len == 0 || len > SYSCALL_MAX_RW_SIZE) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            if (!validate_user_buffer(buf, len, 1)) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            char *kbuf = heap_malloc(len);
            if (!kbuf) {
                tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }

            int bytes = vfs_pread(fd, kbuf, len, offset);
            if (bytes > 0) {
                if (copy_to_user(buf, kbuf, (size_t)bytes) != 0) {
                    bytes = -PERS_ERR_OUT_OF_MEMORY;
                }
            }
            heap_free(kbuf);
            tf->x[0] = (uint64_t)bytes;
            break;
        }

        case SYS_GETDENTS: {
            int fd = (int)(tf->x[0]);
            void *buf = (void *)(tf->x[1]);
            size_t count = (size_t)(tf->x[2]);

            /* The only buffer-taking syscall that had no ceiling: each call
             * pins count bytes of kernel heap for its bounce buffer. */
            if (count == 0 || count > SYSCALL_MAX_RW_SIZE) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            if (!validate_user_buffer(buf, count, 1)) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            void *kbuf = heap_malloc(count);
            if (!kbuf) {
                tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }

            int res = vfs_readdir(fd, kbuf, count);
            if (res > 0) {
                size_t copy_size = (size_t)res * sizeof(struct vfs_dirent);
                if (copy_to_user(buf, kbuf, copy_size) != 0) {
                    res = -PERS_ERR_INVALID_ARGUMENT;
                }
            }
            heap_free(kbuf);
            tf->x[0] = (uint64_t)res;
            break;
        }

        case SYS_CLOSE: {
            int fd = (int)(tf->x[0]);
            tf->x[0] = (uint64_t)vfs_close(fd);
            break;
        }

        case SYS_EXEC: {
            const char *path = (const char *)(tf->x[0]);
            char *const *argv = (char *const *)(tf->x[1]);
            char *const *envp = (char *const *)(tf->x[2]);

            if (!validate_user_buffer(path, 1, 0)) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            char *kpath;
            int err = copy_path_from_user(path, &kpath);
            if (err != PERS_SUCCESS) {
                tf->x[0] = (uint64_t)err;
                break;
            }

            int res = process_exec(kpath, argv, envp);
            heap_free(kpath);

            if (res < 0) {
                tf->x[0] = (uint64_t)res;
                break;
            }

            struct task *curr_task = sched_get_current();
            struct process *execed = curr_task ? process_slot(curr_task->pid) : NULL;
            if (execed) {
                /* Synchronize trap frame after successful image replacement */
                uintptr_t kernel_stack_top = execed->vaddr_kernel_stack + SCHED_TASK_STACK_SIZE;
                struct exception_trap_frame *new_tf =
                    (struct exception_trap_frame *)(kernel_stack_top
                                                    - sizeof(struct exception_trap_frame));
                memcpy(tf, new_tf, sizeof(struct exception_trap_frame));
            }
            break;
        }

        case SYS_FORK: {
            tf->x[0] = (uint64_t)process_fork(tf);
            break;
        }

        case SYS_WAITPID: {
            int wait_pid = (int)tf->x[0];
            int *ustatus = (int *)tf->x[1];
            int options = (int)tf->x[2];
            int kstatus = 0;

            if (ustatus != NULL && !validate_user_buffer(ustatus, sizeof(int), 1)) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            int res = process_waitpid(wait_pid, &kstatus, options);
            if (res >= 0 && ustatus != NULL) {
                if (copy_to_user(ustatus, &kstatus, sizeof(int)) != 0) {
                    tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                    break;
                }
            }
            tf->x[0] = (uint64_t)res;
            break;
        }

        case SYS_PIPE: {
            int *upipefd = (int *)tf->x[0];
            int kpipefd[2];

            if (!validate_user_buffer(upipefd, sizeof(int) * 2, 1)) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            int res = pipe_create(kpipefd);
            if (res == PERS_SUCCESS) {
                if (copy_to_user(upipefd, kpipefd, sizeof(int) * 2) != 0) {
                    tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                    break;
                }
            }
            tf->x[0] = (uint64_t)res;
            break;
        }

        case SYS_DUP2: {
            int oldfd = (int)tf->x[0];
            int newfd = (int)tf->x[1];
            tf->x[0] = (uint64_t)vfs_dup2(oldfd, newfd);
            break;
        }

        case SYS_SIGNAL: {
            int sig = (int)tf->x[0];
            signal_handler_t handler = (signal_handler_t)tf->x[1];

            if (sig >= SIGNAL_COUNT || sig < 1 || sig == SIGNAL_KILL || sig == SIGNAL_STOP) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            signal_handler_t old = proc->signal_handlers[sig - 1].sa_handler;
            proc->signal_handlers[sig - 1].sa_handler = handler;
            proc->signal_handlers[sig - 1].sa_mask = 0;
            proc->signal_handlers[sig - 1].sa_flags = 0;
            proc->signal_handlers[sig - 1].sa_restorer = NULL;

            tf->x[0] = (uint64_t)old;
            break;
        }

        case SYS_KILL: {
            int target_pid = (int)tf->x[0];
            int sig = (int)tf->x[1];

            if (sig < 1 || sig >= SIGNAL_COUNT) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            /*
             * POSIX kill() pid rules:
             * target_pid > 0:  send to process target_pid.
             * target_pid == 0: send to caller's process group (proc->pgid).
             * target_pid == -1: reserved for broadcast; not supported yet, return
             * -PERS_ERR_NO_SUCH_PROCESS. target_pid < -1: send to process group (-target_pid).
             */
            if (target_pid == -1) {
                tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
                break;
            }

            if (target_pid > 0) {
                if (target_pid >= PROCESS_TABLE_SIZE) {
                    tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
                    break;
                }

                struct process *target = process_slot((uint32_t)target_pid);
                if (!target || target->state != PROCESS_STATE_RUNNING) {
                    tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
                    break;
                }

                /* Enforce process hierarchy permissions */
                if (target_pid != (int)pid && target->parent_pid != pid
                    && (int)proc->parent_pid != target_pid && target->pgid != proc->pgid) {
                    tf->x[0] = (uint64_t)-PERS_ERR_PERMISSION_DENIED;
                    break;
                }

                tf->x[0] = (uint64_t)signal_send((uint32_t)target_pid, sig);
            } else if (target_pid == 0) {
                if (proc->pgid == 0) {
                    tf->x[0] = (uint64_t)-PERS_ERR_PERMISSION_DENIED;
                    break;
                }
                tf->x[0] = (uint64_t)signal_send_group(proc->pgid, sig);
            } else { /* target_pid < -1 */
                int64_t raw_pid = target_pid;
                uint64_t abs_pgid = (uint64_t)(-raw_pid);
                if (abs_pgid == 0 || abs_pgid >= PROCESS_TABLE_SIZE) {
                    tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
                    break;
                }

                uint32_t target_pgid = (uint32_t)abs_pgid;

                /* Group permission check: member of group, group matches caller pid, or parent of a
                 * member */
                int allowed = 0;
                if (proc->pgid == target_pgid || proc->pid == target_pgid) {
                    allowed = 1;
                } else {
                    unsigned long irqf = spin_lock_irqsave(&process_table_lock);
                    for (uint32_t i = 1; i < PROCESS_TABLE_SIZE; i++) {
                        struct process *p = process_table[i];
                        if (p && p->state == PROCESS_STATE_RUNNING && p->pgid == target_pgid) {
                            if (p->parent_pid == proc->pid
                                || (int)proc->parent_pid == (int)p->pid) {
                                allowed = 1;
                                break;
                            }
                        }
                    }
                    spin_unlock_irqrestore(&process_table_lock, irqf);
                }

                if (!allowed) {
                    tf->x[0] = (uint64_t)-PERS_ERR_PERMISSION_DENIED;
                    break;
                }

                tf->x[0] = (uint64_t)signal_send_group(target_pgid, sig);
            }
            break;
        }

        case SYS_SIGRETURN: {
            uintptr_t user_frame_ptr = tf->sp_el0;

            if (user_frame_ptr == 0 || user_frame_ptr >= KERNEL_VMA
                || user_frame_ptr + sizeof(struct signal_frame) < user_frame_ptr) {
                goto sigreturn_kill;
            }

            if (!validate_user_buffer((void *)user_frame_ptr, sizeof(struct signal_frame), 0)) {
                goto sigreturn_kill;
            }

            struct signal_frame frame;
            if (copy_from_user(&frame, (void *)user_frame_ptr, sizeof(struct signal_frame)) != 0) {
                goto sigreturn_kill;
            }

            if (frame.saved_tf.elr_el1 >= KERNEL_VMA || frame.saved_tf.sp_el0 >= KERNEL_VMA) {
                goto sigreturn_kill;
            }

            memcpy(tf, &frame.saved_tf, sizeof(struct exception_trap_frame));

            proc->blocked_signals = frame.saved_mask;
            tf->spsr_el1 &= 0xF0000000ULL;
            break;

sigreturn_kill:
            proc->exit_status = -1;
            curr->state = SCHED_TASK_DEAD;
            schedule();
            break;
        }

        case SYS_SIGRESTORE: {
            uintptr_t restorer = (uintptr_t)tf->x[0];
            if (restorer >= KERNEL_VMA) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }
            proc->default_sigrestorer = restorer;
            tf->x[0] = PERS_SUCCESS;
            break;
        }

        case SYS_SIGACTION: {
            int sig = (int)tf->x[0];
            const struct sigaction *uact = (const struct sigaction *)tf->x[1];
            struct sigaction *uoact = (struct sigaction *)tf->x[2];

            if (sig >= SIGNAL_COUNT || sig < 1 || sig == SIGNAL_KILL || sig == SIGNAL_STOP) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            if (uoact) {
                if (!validate_user_buffer(uoact, sizeof(struct sigaction), 1)) {
                    tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                    break;
                }
                if (copy_to_user(uoact, &proc->signal_handlers[sig - 1], sizeof(struct sigaction))
                    != 0) {
                    tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                    break;
                }
            }

            if (uact) {
                if (!validate_user_buffer(uact, sizeof(struct sigaction), 0)) {
                    tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                    break;
                }
                struct sigaction kact;
                if (copy_from_user(&kact, uact, sizeof(struct sigaction)) != 0) {
                    tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                    break;
                }
                proc->signal_handlers[sig - 1] = kact;
                proc->signal_handlers[sig - 1].sa_mask &=
                    ~((1u << (SIGNAL_KILL - 1)) | (1u << (SIGNAL_STOP - 1)));
            }

            tf->x[0] = PERS_SUCCESS;
            break;
        }

        case SYS_SIGPROCMASK: {
            int how = (int)tf->x[0];
            const sigset_t *uset = (const sigset_t *)tf->x[1];
            sigset_t *uoset = (sigset_t *)tf->x[2];

            if (uoset) {
                if (!validate_user_buffer(uoset, sizeof(sigset_t), 1)) {
                    tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                    break;
                }
                if (copy_to_user(uoset, &proc->blocked_signals, sizeof(sigset_t)) != 0) {
                    tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                    break;
                }
            }

            if (uset) {
                if (!validate_user_buffer(uset, sizeof(sigset_t), 0)) {
                    tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                    break;
                }
                sigset_t kset;
                if (copy_from_user(&kset, uset, sizeof(sigset_t)) != 0) {
                    tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                    break;
                }

                if (how == SIG_BLOCK) {
                    proc->blocked_signals |= kset;
                } else if (how == SIG_UNBLOCK) {
                    proc->blocked_signals &= ~kset;
                } else if (how == SIG_SETMASK) {
                    proc->blocked_signals = kset;
                } else {
                    tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                    break;
                }

                proc->blocked_signals &= ~((1u << (SIGNAL_KILL - 1)) | (1u << (SIGNAL_STOP - 1)));
            }

            tf->x[0] = PERS_SUCCESS;
            break;
        }

        case SYS_SIGPENDING: {
            sigset_t *uset = (sigset_t *)tf->x[0];
            if (!validate_user_buffer(uset, sizeof(sigset_t), 1)) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }
            if (copy_to_user(uset, &proc->pending_signals, sizeof(sigset_t)) != 0) {
                tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }
            tf->x[0] = PERS_SUCCESS;
            break;
        }

        case SYS_SIGSUSPEND: {
            const sigset_t *umask = (const sigset_t *)tf->x[0];
            if (!validate_user_buffer(umask, sizeof(sigset_t), 0)) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }
            sigset_t kmask;
            if (copy_from_user(&kmask, umask, sizeof(sigset_t)) != 0) {
                tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }

            sigset_t saved_mask = proc->blocked_signals;
            proc->blocked_signals = kmask;
            proc->blocked_signals &= ~((1u << (SIGNAL_KILL - 1)) | (1u << (SIGNAL_STOP - 1)));

            /* Block until a signal is pending. Mask IRQs across the pending
             * check and the state transition so a signal delivered on this core
             * (e.g. Ctrl-C via the TTY IRQ) cannot be lost between them. */
            for (;;) {
                unsigned long irqf = irq_save();
                if (proc->pending_signals & ~proc->blocked_signals) {
                    irq_restore(irqf);
                    break;
                }
                curr->state = SCHED_TASK_BLOCKED;
                irq_restore(irqf);
                schedule();
            }

            /* POSIX: sigsuspend restores the caller's original mask on return. */
            proc->blocked_signals = saved_mask;
            tf->x[0] = (uint64_t)-PERS_ERR_INTERRUPTED;
            break;
        }

        case SYS_CHDIR: {
            const char *path = (const char *)(tf->x[0]);
            if (!validate_user_buffer(path, 1, 0)) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            char *kpath;
            int err = copy_path_from_user(path, &kpath);
            if (err != PERS_SUCCESS) {
                tf->x[0] = (uint64_t)err;
                break;
            }

            int res = vfs_chdir(kpath);
            heap_free(kpath);
            tf->x[0] = (uint64_t)res;
            break;
        }

        case SYS_GETCWD: {
            char *buf = (char *)tf->x[0];
            size_t size = (size_t)tf->x[1];

            if (!buf || size == 0) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            char *kbuf = heap_malloc(VFS_MAX_PATH_LEN);
            if (!kbuf) {
                tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }

            int res = vfs_getcwd(kbuf, VFS_MAX_PATH_LEN);
            if (res == PERS_SUCCESS) {
                size_t len = strlen(kbuf) + 1;
                if (len > size) {
                    res = -PERS_ERR_INVALID_ARGUMENT;
                } else if (copy_to_user(buf, kbuf, len) != 0) {
                    res = -PERS_ERR_INVALID_ARGUMENT;
                }
            }

            heap_free(kbuf);
            tf->x[0] = (uint64_t)res;
            break;
        }

        case SYS_STAT: {
            const char *upath = (const char *)tf->x[0];
            struct stat *ubuf = (struct stat *)tf->x[1];

            if (!validate_user_buffer(upath, 1, 0)
                || !validate_user_buffer(ubuf, sizeof(struct stat), 1)) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            char *kpath;
            int err = copy_path_from_user(upath, &kpath);
            if (err != PERS_SUCCESS) {
                tf->x[0] = (uint64_t)err;
                break;
            }

            struct stat kbuf;
            int res = vfs_stat(kpath, &kbuf);
            heap_free(kpath);

            if (res == PERS_SUCCESS) {
                if (copy_to_user(ubuf, &kbuf, sizeof(struct stat)) != 0) {
                    res = -PERS_ERR_OUT_OF_MEMORY;
                }
            }
            tf->x[0] = (uint64_t)res;
            break;
        }

        case SYS_MMAP: {
            size_t length = (size_t)tf->x[1];
            int prot = (int)tf->x[2];
            int flags = (int)tf->x[3];
            int fd = (int)tf->x[4];

            if (length == 0 || length > SYSCALL_MAX_MMAP_SIZE) {
                tf->x[0] = (uintptr_t)MAP_FAILED;
                break;
            }

            /*
             * Every mapping must have something behind it. An anonymous request
             * takes no descriptor; a file-backed one needs a vnode that can
             * actually supply pages. Falling through with neither used to hand
             * back an address the caller could only discover was empty by
             * faulting on it.
             */
            int anonymous = (flags & MAP_ANONYMOUS) != 0;
            if (anonymous ? (fd != -1) : (fd < 0 || fd >= VFS_MAX_FDS)) {
                tf->x[0] = (uintptr_t)MAP_FAILED;
                break;
            }

            /* Hold a reference so a concurrent close cannot free the file out
             * from under its own mmap operation. */
            struct vfs_file *file = NULL;
            if (!anonymous) {
                unsigned long fdflags = spin_lock_irqsave(&proc->fd_lock);
                file = proc->fd_table[fd];
                if (file) {
                    atomic_inc(&file->refcount);
                }
                spin_unlock_irqrestore(&proc->fd_lock, fdflags);

                if (!file || !file->node || !file->node->ops || !file->node->ops->mmap) {
                    vfs_file_put(file);
                    tf->x[0] = (uintptr_t)MAP_FAILED;
                    break;
                }
            }

            size_t pages_needed = (length + PAGE_SIZE - 1) / PAGE_SIZE;
            uintptr_t new_region = process_va_alloc(&proc->va, pages_needed);
            if (new_region == 0) {
                vfs_file_put(file);
                tf->x[0] = (uintptr_t)MAP_FAILED;
                break;
            }

            if (!anonymous) {
                int mres = file->node->ops->mmap(file, new_region, length, prot, flags);
                vfs_file_put(file);
                if (mres < 0) {
                    goto mmap_fail;
                }
            }

            if (anonymous) {
                unsigned long mmu_flags = MMU_PTE_VALID | MMU_PTE_PAGE | MMU_PTE_AF
                                          | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_USER
                                          | MMU_PXN | MMU_UXN | MMU_PTE_NG;
                if (!(prot & PROT_WRITE)) {
                    mmu_flags |= MMU_AP_RO;
                }

                for (size_t i = 0; i < pages_needed; i++) {
                    void *kaddr = pmm_alloc_page();
                    if (!kaddr) {
                        goto mmap_fail;
                    }
                    memset(kaddr, 0, PAGE_SIZE);
                    mmu_user_map_page(proc->user_pgd, new_region + i * PAGE_SIZE,
                                      V2P((uintptr_t)kaddr), mmu_flags);
                }
            }
            tf->x[0] = new_region;
            break;

mmap_fail:
            /* Unmap anything mapped so far (frees those pages) and release the
             * reserved VA region so a failed mmap leaks neither. */
            for (size_t j = 0; j < pages_needed; j++) {
                mmu_user_unmap_page(proc->user_pgd, new_region + j * PAGE_SIZE);
            }
            process_va_free(&proc->va, new_region);
            tf->x[0] = (uintptr_t)MAP_FAILED;
            break;
        }

        case SYS_LSEEK: {
            int fd = (int)tf->x[0];
            off_t offset = (off_t)tf->x[1];
            int whence = (int)tf->x[2];
            tf->x[0] = (uint64_t)vfs_lseek(fd, offset, whence);
            break;
        }

        case SYS_SYNC: {
            pagecache_sync();
            block_cache_sync();
            tf->x[0] = PERS_SUCCESS;
            break;
        }
        case SYS_MKDIR: {
            const char *path = (const char *)tf->x[0];
            if (!validate_user_buffer(path, 1, 0)) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }
            char *kpath;
            int err = copy_path_from_user(path, &kpath);
            if (err != PERS_SUCCESS) {
                tf->x[0] = (uint64_t)err;
                break;
            }
            tf->x[0] = vfs_mkdir(kpath);
            heap_free(kpath);
            break;
        }
        case SYS_RMDIR: {
            const char *path = (const char *)tf->x[0];
            if (!validate_user_buffer(path, 1, 0)) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }
            char *kpath;
            int err = copy_path_from_user(path, &kpath);
            if (err != PERS_SUCCESS) {
                tf->x[0] = (uint64_t)err;
                break;
            }
            tf->x[0] = vfs_rmdir(kpath);
            heap_free(kpath);
            break;
        }
        case SYS_UNLINK: {
            const char *path = (const char *)tf->x[0];
            if (!validate_user_buffer(path, 1, 0)) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }
            char *kpath;
            int err = copy_path_from_user(path, &kpath);
            if (err != PERS_SUCCESS) {
                tf->x[0] = (uint64_t)err;
                break;
            }
            tf->x[0] = vfs_unlink(kpath);
            heap_free(kpath);
            break;
        }
        case SYS_RENAME: {
            const char *oldpath = (const char *)tf->x[0];
            const char *newpath = (const char *)tf->x[1];
            if (!validate_user_buffer(oldpath, 1, 0) || !validate_user_buffer(newpath, 1, 0)) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }
            char *koldpath;
            int err = copy_path_from_user(oldpath, &koldpath);
            if (err != PERS_SUCCESS) {
                tf->x[0] = (uint64_t)err;
                break;
            }

            char *knewpath;
            err = copy_path_from_user(newpath, &knewpath);
            if (err != PERS_SUCCESS) {
                heap_free(koldpath);
                tf->x[0] = (uint64_t)err;
                break;
            }
            tf->x[0] = vfs_rename(koldpath, knewpath);
            heap_free(koldpath);
            heap_free(knewpath);
            break;
        }

        case SYS_FSYNC: {
            int fd = (int)tf->x[0];
            tf->x[0] = (uint64_t)vfs_fsync(fd);
            break;
        }
        case SYS_FCNTL: {
            int fd = (int)tf->x[0];
            int cmd = (int)tf->x[1];
            int arg = (int)tf->x[2];

            if (fd < 0 || fd >= VFS_MAX_FDS) {
                tf->x[0] = (uint64_t)-PERS_ERR_BAD_FILE_DESCRIPTOR;
                break;
            }

            unsigned long fdflags = spin_lock_irqsave(&proc->fd_lock);
            struct vfs_file *f = proc->fd_table[fd];
            if (!f) {
                spin_unlock_irqrestore(&proc->fd_lock, fdflags);
                tf->x[0] = (uint64_t)-PERS_ERR_BAD_FILE_DESCRIPTOR;
                break;
            }

            int ret = 0;
            switch (cmd) {
                case VFS_F_GETFD:
                    ret = proc->fd_flags[fd];
                    break;
                case VFS_F_SETFD:
                    proc->fd_flags[fd] = arg;
                    break;
                case VFS_F_GETFL:
                    ret = f->flags;
                    break;
                case VFS_F_SETFL:
                    f->flags = (f->flags & VFS_O_ACCMODE) | (arg & ~VFS_O_ACCMODE);
                    break;
                default:
                    ret = -PERS_ERR_INVALID_ARGUMENT;
                    break;
            }
            spin_unlock_irqrestore(&proc->fd_lock, fdflags);

            tf->x[0] = (uint64_t)ret;
            break;
        }

        case SYS_SETPGID: {
            int target_pid = (int)tf->x[0];
            int new_pgid = (int)tf->x[1];

            int curr_pid = process_find_current();
            if (curr_pid < 0) {
                tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
                break;
            }

            if (target_pid == 0) {
                target_pid = curr_pid;
            }
            if (new_pgid == 0) {
                new_pgid = target_pid;
            }

            /* A pgid is always some process's pid, so it carries the same bound. */
            if (target_pid < 1 || target_pid >= PROCESS_TABLE_SIZE || new_pgid < 1
                || new_pgid >= PROCESS_TABLE_SIZE) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            unsigned long irqf = spin_lock_irqsave(&process_table_lock);
            struct process *target_proc = process_table[target_pid];
            if (!target_proc || target_proc->state != PROCESS_STATE_RUNNING) {
                spin_unlock_irqrestore(&process_table_lock, irqf);
                tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
                break;
            }

            /* Restrict setpgid to self or direct child */
            if (target_pid != curr_pid && (int)target_proc->parent_pid != curr_pid) {
                spin_unlock_irqrestore(&process_table_lock, irqf);
                tf->x[0] = (uint64_t)-PERS_ERR_PERMISSION_DENIED;
                break;
            }

            /* A parent may only place a child before it execs; the new image
             * owns its own membership afterwards. Moving yourself stays legal. */
            if (target_pid != curr_pid && target_proc->has_execed) {
                spin_unlock_irqrestore(&process_table_lock, irqf);
                tf->x[0] = (uint64_t)-PERS_ERR_PERMISSION_DENIED;
                break;
            }

            /* A session leader's pid names its session, so it cannot also name a
             * group elsewhere. */
            if (target_proc->sid == target_proc->pid) {
                spin_unlock_irqrestore(&process_table_lock, irqf);
                tf->x[0] = (uint64_t)-PERS_ERR_PERMISSION_DENIED;
                break;
            }

            /* Groups do not span sessions; an empty group is only legal when the
             * target is creating its own. */
            uint32_t group_sid = pgid_session_locked((uint32_t)new_pgid);
            if (group_sid == 0) {
                if (new_pgid != target_pid) {
                    spin_unlock_irqrestore(&process_table_lock, irqf);
                    tf->x[0] = (uint64_t)-PERS_ERR_PERMISSION_DENIED;
                    break;
                }
            } else if (group_sid != target_proc->sid) {
                spin_unlock_irqrestore(&process_table_lock, irqf);
                tf->x[0] = (uint64_t)-PERS_ERR_PERMISSION_DENIED;
                break;
            }

            target_proc->pgid = (uint32_t)new_pgid;
            spin_unlock_irqrestore(&process_table_lock, irqf);
            tf->x[0] = PERS_SUCCESS;
            break;
        }

        case SYS_GETPGID: {
            int target_pid = (int)tf->x[0];
            int curr_pid = process_find_current();
            if (curr_pid < 0) {
                tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
                break;
            }
            if (target_pid == 0) {
                target_pid = curr_pid;
            }
            if (target_pid < 0 || target_pid >= PROCESS_TABLE_SIZE) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            unsigned long irqf = spin_lock_irqsave(&process_table_lock);
            struct process *target_proc = process_table[target_pid];
            if (!target_proc || target_proc->state != PROCESS_STATE_RUNNING) {
                spin_unlock_irqrestore(&process_table_lock, irqf);
                tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
                break;
            }
            uint32_t res_pgid = target_proc->pgid;
            spin_unlock_irqrestore(&process_table_lock, irqf);
            tf->x[0] = (uint64_t)res_pgid;
            break;
        }

        case SYS_TCSETPGRP: {
            int fd = (int)tf->x[0];
            int new_pgid = (int)tf->x[1];

            int curr_pid = process_find_current();
            if (curr_pid < 0 || fd < 0 || fd >= VFS_MAX_FDS || new_pgid < 1
                || new_pgid >= PROCESS_TABLE_SIZE) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            struct process *curr_proc = process_slot((uint32_t)curr_pid);
            if (!curr_proc) {
                tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
                break;
            }

            unsigned long fdflags = spin_lock_irqsave(&curr_proc->fd_lock);
            struct vfs_file *file = curr_proc->fd_table[fd];
            if (!file || !file->node || file->node->ops != &devfs_tty_ops) {
                spin_unlock_irqrestore(&curr_proc->fd_lock, fdflags);
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }
            /* Note: internal_info is dereferenced here after dropping fd_lock.
             * Safe because console_tty is a static global struct tty that is never freed. */
            struct tty *tty = (struct tty *)file->node->internal_info;
            spin_unlock_irqrestore(&curr_proc->fd_lock, fdflags);

            if (!tty) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            /* Resolve sessions under the table lock, then take tty->lock -- never
             * both at once. tty->lock is held in the UART interrupt, so nesting
             * the two would fix an ordering between them permanently. */
            unsigned long irqf = spin_lock_irqsave(&process_table_lock);
            uint32_t caller_sid = curr_proc->sid;
            int caller_is_leader = curr_proc->sid == curr_proc->pid;
            uint32_t group_sid = pgid_session_locked((uint32_t)new_pgid);
            spin_unlock_irqrestore(&process_table_lock, irqf);

            if (group_sid == 0 || group_sid != caller_sid) {
                tf->x[0] = (uint64_t)-PERS_ERR_PERMISSION_DENIED;
                break;
            }

            /* Handing the terminal on from a background group stops the caller,
             * which is why a job-control shell ignores SIGTTOU: that is what lets
             * it take the terminal back once a job finishes. */
            if (tty_access_check(tty, SIGNAL_TTOU) == TTY_ACCESS_STOPPED) {
                tf->x[0] = (uint64_t)-PERS_ERR_INTERRUPTED;
                break;
            }

            unsigned long ttyflags = spin_lock_irqsave(&tty->lock);
            /* An unowned terminal goes to the first session leader that asks;
             * after that only that session may steer it. POSIX acquires on
             * open() instead, which would put ownership in the open path. */
            if (tty->session_id == 0 && caller_is_leader) {
                tty->session_id = caller_sid;
            }
            if (tty->session_id != caller_sid) {
                spin_unlock_irqrestore(&tty->lock, ttyflags);
                tf->x[0] = (uint64_t)-PERS_ERR_PERMISSION_DENIED;
                break;
            }
            tty->foreground_pgid = (uint32_t)new_pgid;
            spin_unlock_irqrestore(&tty->lock, ttyflags);

            tf->x[0] = PERS_SUCCESS;
            break;
        }

        case SYS_TCGETPGRP: {
            int fd = (int)tf->x[0];
            int curr_pid = process_find_current();
            if (curr_pid < 0 || fd < 0 || fd >= VFS_MAX_FDS) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            struct process *curr_proc = process_slot((uint32_t)curr_pid);
            if (!curr_proc) {
                tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
                break;
            }

            unsigned long fdflags = spin_lock_irqsave(&curr_proc->fd_lock);
            struct vfs_file *file = curr_proc->fd_table[fd];
            if (!file || !file->node || file->node->ops != &devfs_tty_ops) {
                spin_unlock_irqrestore(&curr_proc->fd_lock, fdflags);
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }
            struct tty *tty = (struct tty *)file->node->internal_info;
            spin_unlock_irqrestore(&curr_proc->fd_lock, fdflags);

            if (!tty) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            unsigned long ttyflags = spin_lock_irqsave(&tty->lock);
            uint32_t fg_pgid = tty->foreground_pgid;
            spin_unlock_irqrestore(&tty->lock, ttyflags);

            tf->x[0] = (uint64_t)fg_pgid;
            break;
        }

        case SYS_SETSID: {
            int curr_pid = process_find_current();
            if (curr_pid < 0) {
                tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
                break;
            }

            unsigned long irqf = spin_lock_irqsave(&process_table_lock);
            struct process *curr_p = process_table[curr_pid];
            if (!curr_p || curr_p->state != PROCESS_STATE_RUNNING) {
                spin_unlock_irqrestore(&process_table_lock, irqf);
                tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
                break;
            }

            /* Fails if caller is already a process group leader (pgid == pid) */
            if (curr_p->pgid == curr_p->pid) {
                spin_unlock_irqrestore(&process_table_lock, irqf);
                tf->x[0] = (uint64_t)-PERS_ERR_PERMISSION_DENIED;
                break;
            }

            /* The old session keeps its terminal: ownership is a property of the
             * session, and only the caller is leaving. A session leader cannot
             * reach here, so an owning session can never lose its leader this way. */
            curr_p->sid = curr_p->pid;
            curr_p->pgid = curr_p->pid;
            uint32_t new_sid = curr_p->sid;
            spin_unlock_irqrestore(&process_table_lock, irqf);

            tf->x[0] = (uint64_t)new_sid;
            break;
        }

        case SYS_GETSID: {
            int target_pid = (int)tf->x[0];
            int curr_pid = process_find_current();
            if (curr_pid < 0) {
                tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
                break;
            }
            if (target_pid == 0) {
                target_pid = curr_pid;
            }
            if (target_pid < 0 || target_pid >= PROCESS_TABLE_SIZE) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            unsigned long irqf = spin_lock_irqsave(&process_table_lock);
            struct process *target_proc = process_table[target_pid];
            if (!target_proc || target_proc->state != PROCESS_STATE_RUNNING) {
                spin_unlock_irqrestore(&process_table_lock, irqf);
                tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
                break;
            }
            uint32_t res_sid = target_proc->sid;
            spin_unlock_irqrestore(&process_table_lock, irqf);
            tf->x[0] = (uint64_t)res_sid;
            break;
        }

        default: {
            pr_warn("syscall: unknown syscall: %lu\n", syscall_nr);
            break;
        }
    }
}
