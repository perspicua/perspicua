/*
 * syscall.c - Implementation of the system call dispatcher.
 *
 * This file handles the validation of user-provided buffers and dispatches
 * system calls to their respective kernel implementations such as filesystem,
 * process management, and scheduling operations.
 */

#include "core/syscall.h"

#include "core/signals.h"
#include "fs/vfs.h"
#include "mm/pmm.h"
#include "uapi/mman.h"
#include "uapi/syscalls.h"
#include "uapi/errors.h"

#include "arch/uaccess.h"

#include "driver/uart.h"
#include "sched/process.h"
#include "mm/mmu.h"
#include "mm/addr.h"
#include "mm/heap.h"
#include "fs/pipe.h"
#include "stdio.h"
#include "string.h"
#include "panic.h"
#include "core/signals.h"

/*
 * validate_user_buffer - Verifies that a memory range provided by a user
 * process is valid, belongs to user-space, and has appropriate permissions.
 */
int validate_user_buffer(const void* ptr, size_t len, int writable)
{
    if (!ptr || len == 0)
        return 0;
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end = start + len;

    /* Ensure the range does not wrap around or intrude into kernel space */
    if (end < start || end > KERNEL_VMA)
    {
        return 0;
    }

    int pid = process_find_current();
    if (pid < 0)
    {
        return 0;
    }

    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    unsigned long* pgd = process_table[pid].user_pgd;
    if (!pgd)
    {
        spin_unlock_irqrestore(&process_table_lock, flags);
        return 0;
    }

    /* Iterate through every page in the buffer range to check permissions */
    uintptr_t curr = start & ~0xFFFUL;
    while (curr < end)
    {
        unsigned long current_flags;
        if (!mmu_user_query(pgd, curr, NULL, &current_flags))
        {
            spin_unlock_irqrestore(&process_table_lock, flags);
            return 0;
        }

        /* Must be a user-accessible page */
        if (!(current_flags & MMU_AP_USER))
        {
            spin_unlock_irqrestore(&process_table_lock, flags);
            return 0;
        }

        /* Check for write permission if requested (AP[2] bit 7 is Read-Only) */
        if (writable && (current_flags & (1ULL << 7)))
        {
            spin_unlock_irqrestore(&process_table_lock, flags);
            return 0;
        }

        curr += 4096;
    }
    spin_unlock_irqrestore(&process_table_lock, flags);

    return 1;
}

/*
 * syscall_handle - The primary entry point for all synchronous exceptions from EL0.
 */
void syscall_handle(struct exception_trap_frame* tf)
{
    uint64_t syscall_nr = tf->x[8];
    struct task* curr = sched_get_current();
    uint32_t pid = curr->pid;

    switch (syscall_nr)
    {
    case SYS_WRITE:
    { /* sys_write(int fd, const char* buf, size_t len) */
        int fd = (int)(tf->x[0]);
        const char* buf = (const char*)(tf->x[1]);
        size_t len = (size_t)(tf->x[2]);

        /* Enforce a per-syscall maximum to prevent heap exhaustion */
        if (len == 0 || len > SYSCALL_MAX_RW_SIZE)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }
        if (!validate_user_buffer(buf, len, 0))
        {
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }
        char* kbuf = heap_malloc(len);
        if (!kbuf)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
            break;
        }
        if (copy_from_user(kbuf, buf, len) != 0)
        {
            heap_free(kbuf);
            tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
            break;
        }
        int bytes = vfs_write(fd, kbuf, len);
        heap_free(kbuf);
        tf->x[0] = (uint64_t)bytes;
        break;
    }

    case SYS_EXIT:
    { /* sys_exit(int status) */
        int status = (int)tf->x[0];
        process_exit(pid, status);
        curr->state = SCHED_TASK_DEAD;
        schedule();
        break;
    }

    case SYS_GETPID:
    { /* sys_getpid() */
        tf->x[0] = (uint64_t)pid;
        break;
    }

    case SYS_YIELD:
    { /* sys_yield() */
        schedule();
        break;
    }

    case SYS_SLEEP:
    { /* sys_sleep(ms) */
        unsigned long ms = tf->x[0];
        sched_sleep_ms(ms);
        break;
    }

    case SYS_OPEN:
    { /* sys_open(const char* path, int flags) */
        const char* path = (const char*)(tf->x[0]);
        int flags = (int)(tf->x[1]);

        char* kpath = heap_malloc(VFS_MAX_PATH_LEN);
        long copied = strncpy_from_user(kpath, path, VFS_MAX_PATH_LEN);
        if (copied < 0 || copied >= VFS_MAX_PATH_LEN)
        {
            heap_free(kpath);
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }

        int fd = vfs_open(kpath, flags);
        heap_free(kpath);
        tf->x[0] = (uint64_t)fd;
        break;
    }

    case SYS_READ:
    { /* sys_read(int fd, char* buf, size_t len) */
        int fd = (int)(tf->x[0]);
        char* buf = (char*)(tf->x[1]);
        size_t len = (size_t)(tf->x[2]);

        if (len == 0 || len > SYSCALL_MAX_RW_SIZE)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }
        if (!validate_user_buffer(buf, len, 1))
        {
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }
        char* kbuf = heap_malloc(len);
        if (!kbuf)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
            break;
        }
        int bytes = vfs_read(fd, kbuf, len);
        if (bytes > 0)
        {
            if (copy_to_user(buf, kbuf, (size_t)bytes) != 0)
                bytes = -PERS_ERR_OUT_OF_MEMORY;
        }
        heap_free(kbuf);
        tf->x[0] = (uint64_t)bytes;
        break;
    }

    case SYS_GETDENTS:
    { /* sys_getdents(int fd, void* buf, size_t count) */
        int fd = (int)(tf->x[0]);
        void* buf = (void*)(tf->x[1]);
        size_t count = (size_t)(tf->x[2]);

        if (!validate_user_buffer(buf, count, 1))
        {
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }

        /* count in this context is often sizeof(struct vfs_dirent) */
        void* kbuf = heap_malloc(count);
        if (!kbuf)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
            break;
        }

        int res = vfs_readdir(fd, kbuf, count);
        if (res > 0)
        {
            size_t copy_size = (size_t)res * sizeof(struct vfs_dirent);
            if (copy_to_user(buf, kbuf, copy_size) != 0)
            {
                res = -PERS_ERR_INVALID_ARGUMENT;
            }
        }
        heap_free(kbuf);
        tf->x[0] = (uint64_t)res;
        break;
    }

    case SYS_CLOSE:
    { /* sys_close(int fd) */
        int fd = (int)(tf->x[0]);
        tf->x[0] = (uint64_t)vfs_close(fd);
        break;
    }

    case SYS_EXEC:
    { /* sys_exec(const char* path) */
        const char* path = (const char*)(tf->x[0]);
        if (!validate_user_buffer(path, 1, 0))
        {
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }

        char* kpath = heap_malloc(VFS_MAX_PATH_LEN);
        long copied = strncpy_from_user(kpath, path, VFS_MAX_PATH_LEN);
        if (copied < 0 || copied >= VFS_MAX_PATH_LEN)
        {
            heap_free(kpath);
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }

        int res = process_exec(kpath);
        heap_free(kpath);

        if (res < 0)
        {
            tf->x[0] = (uint64_t)res;
            break;
        }

        struct task* curr_task = sched_get_current();
        if (curr_task)
        {
            // The new trap frame was built by process_exec — copy it into
            // the live tf so restore_all uses the right ELR/SP/SPSR
            uintptr_t kernel_stack_top = process_table[curr_task->pid].vaddr_kernel_stack + SCHED_TASK_STACK_SIZE;
            struct exception_trap_frame* new_tf =
                (struct exception_trap_frame*)(kernel_stack_top - sizeof(struct exception_trap_frame));
            memcpy(tf, new_tf, sizeof(struct exception_trap_frame));
        }
        // Do NOT set tf->x[0] — the new tf already has x[0]=0 from memset
        break;
    }

    case SYS_FORK:
    { /* sys_fork() */
        tf->x[0] = (uint64_t)process_fork(tf);
        break;
    }

    case SYS_WAITPID:
    { /* sys_waitpid(int pid, int* status, int options) */
        int wait_pid = (int)tf->x[0];
        int* ustatus = (int*)tf->x[1];
        int options = (int)tf->x[2];
        int kstatus = 0;

        if (ustatus != NULL && !validate_user_buffer(ustatus, sizeof(int), 1))
        {
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }

        int res = process_waitpid(wait_pid, &kstatus, options);
        if (res >= 0 && ustatus != NULL)
        {
            if (copy_to_user(ustatus, &kstatus, sizeof(int)) != 0)
            {
                tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }
        }
        tf->x[0] = (uint64_t)res;
        break;
    }

    case SYS_PIPE:
    { /* sys_pipe(int pipefd[2]) */
        int* upipefd = (int*)tf->x[0];
        int kpipefd[2];

        if (!validate_user_buffer(upipefd, sizeof(int) * 2, 1))
        {
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }

        int res = kernel_pipe(kpipefd);
        if (res == PERS_SUCCESS)
        {
            if (copy_to_user(upipefd, kpipefd, sizeof(int) * 2) != 0)
            {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }
        }
        tf->x[0] = (uint64_t)res;
        break;
    }

    case SYS_DUP2:
    { /* sys_dup2(int oldfd, int newfd) */
        int oldfd = (int)tf->x[0];
        int newfd = (int)tf->x[1];
        tf->x[0] = (uint64_t)vfs_dup2(oldfd, newfd);
        break;
    }

    case SYS_SIGNAL:
    { /* sys_signal(int sig, signal_handler_t handler) */
        int sig = (int)tf->x[0];
        signal_handler_t handler = (signal_handler_t)tf->x[1];

        if (sig >= SIGNAL_COUNT || sig < 1 || sig == SIGNAL_KILL || sig == SIGNAL_STOP)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }

        int curr_process_pid = process_find_current();
        if (curr_process_pid < 0)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
            break;
        }

        struct process* curr_process = &process_table[curr_process_pid];

        signal_handler_t old = curr_process->signal_handlers[sig - 1].sa_handler;
        curr_process->signal_handlers[sig - 1].sa_handler = handler;
        curr_process->signal_handlers[sig - 1].sa_mask = 0;
        curr_process->signal_handlers[sig - 1].sa_flags = 0;
        curr_process->signal_handlers[sig - 1].sa_restorer = NULL;

        tf->x[0] = (uint64_t)old;
        break;
    }

    case SYS_KILL:
    {
        int target_pid = (int)tf->x[0];
        int sig = (int)tf->x[1];

        if (sig < 1 || sig >= SIGNAL_COUNT)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }
        if (target_pid == 0)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_PERMISSION_DENIED;
            break;
        }
        if (target_pid >= PROCESS_TABLE_SIZE || process_table[target_pid].state == PROCESS_STATE_EMPTY)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
            break;
        }

        /* Permission check: can only kill self, children, or parent */
        if (target_pid > 0 && target_pid < PROCESS_TABLE_SIZE)
        {
            if (target_pid != (int)pid && process_table[target_pid].parent_pid != pid
                && (int)process_table[pid].parent_pid != target_pid)
            {
                tf->x[0] = (uint64_t)-PERS_ERR_PERMISSION_DENIED;
                break;
            }
        }

        tf->x[0] = (uint64_t)signal_send(target_pid, sig);
        break;
    }
    case SYS_SIGRETURN:
    {
        uintptr_t user_frame_ptr = tf->sp_el0;

        if (user_frame_ptr == 0 || user_frame_ptr >= KERNEL_VMA
            || user_frame_ptr + sizeof(struct signal_frame) < user_frame_ptr)
        {
            goto sigreturn_kill;
        }

        if (!validate_user_buffer((void*)user_frame_ptr, sizeof(struct signal_frame), 0))
            goto sigreturn_kill;

        struct signal_frame frame;
        if (copy_from_user(&frame, (void*)user_frame_ptr, sizeof(struct signal_frame)) != 0)
            goto sigreturn_kill;

        if (frame.saved_tf.elr_el1 >= KERNEL_VMA)
            goto sigreturn_kill;
        if (frame.saved_tf.sp_el0 >= KERNEL_VMA)
            goto sigreturn_kill;

        memcpy(tf, &frame.saved_tf, sizeof(struct exception_trap_frame));

        /* Restore signal mask */
        process_table[pid].blocked_signals = frame.saved_mask;

        tf->spsr_el1 &= 0xF0000000ULL;

        break;

sigreturn_kill:
        process_table[pid].exit_status = -1;
        curr->state = SCHED_TASK_DEAD;
        schedule();
        break;
    }
    case SYS_SIGRESTORE:
    {
        uintptr_t restorer = (uintptr_t)tf->x[0];
        if (restorer >= KERNEL_VMA)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }
        process_table[pid].default_sigrestorer = restorer;
        tf->x[0] = PERS_SUCCESS;
        break;
    }
    case SYS_SIGACTION:
    { /* sys_sigaction(int sig, const struct sigaction *act, struct sigaction *oact) */
        int sig = (int)tf->x[0];
        const struct sigaction* uact = (const struct sigaction*)tf->x[1];
        struct sigaction* uoact = (struct sigaction*)tf->x[2];

        if (sig >= SIGNAL_COUNT || sig < 1 || sig == SIGNAL_KILL || sig == SIGNAL_STOP)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }

        struct process* p = &process_table[pid];

        if (uoact)
        {
            if (!validate_user_buffer(uoact, sizeof(struct sigaction), 1))
            {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }
            if (copy_to_user(uoact, &p->signal_handlers[sig - 1], sizeof(struct sigaction)) != 0)
            {
                tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }
        }

        if (uact)
        {
            if (!validate_user_buffer(uact, sizeof(struct sigaction), 0))
            {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }
            struct sigaction kact;
            if (copy_from_user(&kact, uact, sizeof(struct sigaction)) != 0)
            {
                tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }
            p->signal_handlers[sig - 1] = kact;
            /* Ensure KILL and STOP cannot be blocked in sa_mask */
            p->signal_handlers[sig - 1].sa_mask &= ~((1u << (SIGNAL_KILL - 1)) | (1u << (SIGNAL_STOP - 1)));
        }

        tf->x[0] = PERS_SUCCESS;
        break;
    }
    case SYS_SIGPROCMASK:
    { /* sys_sigprocmask(int how, const sigset_t *set, sigset_t *oset) */
        int how = (int)tf->x[0];
        const sigset_t* uset = (const sigset_t*)tf->x[1];
        sigset_t* uoset = (sigset_t*)tf->x[2];

        struct process* p = &process_table[pid];

        if (uoset)
        {
            if (!validate_user_buffer(uoset, sizeof(sigset_t), 1))
            {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }
            if (copy_to_user(uoset, &p->blocked_signals, sizeof(sigset_t)) != 0)
            {
                tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }
        }

        if (uset)
        {
            if (!validate_user_buffer(uset, sizeof(sigset_t), 0))
            {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }
            sigset_t kset;
            if (copy_from_user(&kset, uset, sizeof(sigset_t)) != 0)
            {
                tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }

            if (how == SIG_BLOCK)
            {
                p->blocked_signals |= kset;
            }
            else if (how == SIG_UNBLOCK)
            {
                p->blocked_signals &= ~kset;
            }
            else if (how == SIG_SETMASK)
            {
                p->blocked_signals = kset;
            }
            else
            {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            /* Ensure KILL and STOP cannot be blocked */
            p->blocked_signals &= ~((1u << (SIGNAL_KILL - 1)) | (1u << (SIGNAL_STOP - 1)));
        }

        tf->x[0] = PERS_SUCCESS;
        break;
    }
    case SYS_SIGPENDING:
    { /* sys_sigpending(sigset_t *set) */
        sigset_t* uset = (sigset_t*)tf->x[0];
        if (!validate_user_buffer(uset, sizeof(sigset_t), 1))
        {
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }
        if (copy_to_user(uset, &process_table[pid].pending_signals, sizeof(sigset_t)) != 0)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
            break;
        }
        tf->x[0] = PERS_SUCCESS;
        break;
    }
    case SYS_SIGSUSPEND:
    { /* sys_sigsuspend(const sigset_t *mask) */
        const sigset_t* umask = (const sigset_t*)tf->x[0];
        if (!validate_user_buffer(umask, sizeof(sigset_t), 0))
        {
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }
        sigset_t kmask;
        if (copy_from_user(&kmask, umask, sizeof(sigset_t)) != 0)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
            break;
        }

        struct process* p = &process_table[pid];
        p->blocked_signals = kmask;
        /* Ensure KILL and STOP cannot be blocked */
        p->blocked_signals &= ~((1u << (SIGNAL_KILL - 1)) | (1u << (SIGNAL_STOP - 1)));

        /* Wait for a signal */
        while (!(p->pending_signals & ~p->blocked_signals))
        {
            /* Fix lost-wakeup: set state to BLOCKED before yielding.
             * If a signal arrives after this but before schedule(),
             * sched_unblock() will see BLOCKED and re-ready us. */
            curr->state = SCHED_TASK_BLOCKED;
            schedule();
        }

        /* The signal will be delivered by ret_to_user -> signal_handle_pending */
        /* But we need to make sure sigreturn restores the OLD mask, not the sigsuspend mask.
         * This is tricky because signal_handle_pending will save the CURRENT mask (which is kmask).
         * Standard sigsuspend says the signal mask is restored after the handler returns.
         * So signal_handle_pending should save the OLD mask if we are in sigsuspend?
         * Actually, signal_handle_pending always saves the mask that was in effect before the handler was called.
         * For sigsuspend, it IS the kmask.
         * Wait, standard sigsuspend: "The signal mask of the process is restored to its previous value when
         * sigsuspend() returns." If a signal is delivered, sigsuspend returns after the signal handler returns. So the
         * sigreturn should restore the mask that was in effect BEFORE sigsuspend.
         */

        /* I'll use a hack for now: signal_handle_pending will be called after this syscall returns to user mode,
         * but before any user code runs.
         * If I change p->blocked_signals here, it will be saved by signal_handle_pending.
         * To restore old_mask, I'd need to save it somewhere.
         */

        tf->x[0] = (uint64_t)-PERS_ERR_INTERRUPTED;  // sigsuspend always returns -1/EINTR
        break;
    }

    case SYS_CHDIR:
    {
        const char* path = (const char*)(tf->x[0]);
        if (!validate_user_buffer(path, 1, 0))
        {
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }

        char* kpath = heap_malloc(VFS_MAX_PATH_LEN);
        if (!kpath)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
            break;
        }

        long copied = strncpy_from_user(kpath, path, VFS_MAX_PATH_LEN);
        if (copied < 0 || copied >= VFS_MAX_PATH_LEN)
        {
            heap_free(kpath);
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }

        int res = vfs_chdir(kpath);
        heap_free(kpath);
        tf->x[0] = (uint64_t)res;
        break;
    }
    case SYS_GETCWD:
    {
        char* buf = (char*)tf->x[0];
        size_t size = (size_t)tf->x[1];

        if (!buf || size == 0)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }

        char* kbuf = heap_malloc(VFS_MAX_PATH_LEN);
        if (!kbuf)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
            break;
        }

        int res = vfs_getcwd(kbuf, VFS_MAX_PATH_LEN);
        if (res == PERS_SUCCESS)
        {
            size_t len = strlen(kbuf) + 1;
            if (len > size)
            {
                res = -PERS_ERR_INVALID_ARGUMENT;
            }
            else if (copy_to_user(buf, kbuf, len) != 0)
            {
                res = -PERS_ERR_INVALID_ARGUMENT;
            }
        }

        heap_free(kbuf);
        tf->x[0] = (uint64_t)res;
        break;
    }
    case SYS_MMAP:
    {  // void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
        uintptr_t addr = (uintptr_t)tf->x[0];
        size_t length = (size_t)tf->x[1];
        int prot = (int)tf->x[2];
        int flags = (int)tf->x[3];
        int fd = (int)tf->x[4];
        off_t offset = (off_t)tf->x[5];

        (void)addr;
        (void)offset;

        int curr_process_pid = process_find_current();
        if (curr_process_pid < 0)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
            break;
        }

        struct process* curr_process = &process_table[curr_process_pid];

        if (length == 0 || length > SYSCALL_MAX_MMAP_SIZE)
        {
            tf->x[0] = (uintptr_t)MAP_FAILED;
            break;
        }

        /* Safe page-count rounding — length is bounded, no overflow possible */
        size_t pages_needed = (length + PAGE_SIZE - 1) / PAGE_SIZE;
        uintptr_t new_region = process_va_alloc(&curr_process->va, pages_needed);
        if (new_region == 0)
        {
            tf->x[0] = (uintptr_t)MAP_FAILED;
            break;
        }

        if (fd != -1)
        {
            struct vfs_file* file = curr_process->fd_table[fd];
            if (file == NULL)
            {
                tf->x[0] = (uintptr_t)MAP_FAILED;
                break;
            }
            if (file->node->ops->mmap != NULL)
            {
                int rc = file->node->ops->mmap(file, new_region, length, prot, flags);
                if (rc < 0)
                {
                    tf->x[0] = (uintptr_t)MAP_FAILED;
                    break;
                }
            }
        }
        if (flags & MAP_ANONYMOUS)
        {
            unsigned long mmu_flags =
                MMU_PTE_VALID | MMU_PTE_PAGE | MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_USER | MMU_PXN;
            if (!(prot & PROT_WRITE))
            {
                mmu_flags |= MMU_AP_RO;
            }

            mmu_flags |= MMU_UXN;
            for (size_t i = 0; i < pages_needed; i++)
            {
                void* kaddr = pmm_alloc_page();
                if (!kaddr)
                {
                    // For now, fail if any page allocation fails.
                    // Ideally we'd rollback here, but MAP_FAILED signals the error.
                    tf->x[0] = (uintptr_t)MAP_FAILED;
                    goto mmap_done;
                }
                memset(kaddr, 0, PAGE_SIZE);
                uintptr_t phys = V2P((uintptr_t)kaddr);
                mmu_user_map_page(curr_process->user_pgd, new_region + i * PAGE_SIZE, phys, mmu_flags);
            }
        }
        tf->x[0] = new_region;
mmap_done:
        break;
    }

    default:
    {
        pr_warn("syscall: unknown syscall: %lu\n", syscall_nr);
        break;
    }
    }
}
