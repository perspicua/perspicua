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
#include "fs/vfs.h"
#include "fs/pipe.h"
#include "mm/pmm.h"
#include "mm/mmu.h"
#include "mm/addr.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "sched/process.h"
#include "driver/uart.h"

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

    int pid = process_find_current();
    if (pid < 0) {
        return 0;
    }

    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    unsigned long *pgd = process_table[pid].user_pgd;
    if (!pgd) {
        spin_unlock_irqrestore(&process_table_lock, flags);
        return 0;
    }

    uintptr_t curr = start & ~0xFFFULL;
    while (curr < end) {
        unsigned long current_flags;
        if (!mmu_user_query(pgd, curr, NULL, &current_flags)) {
            spin_unlock_irqrestore(&process_table_lock, flags);
            return 0;
        }

        /* Check for user-mode accessibility */
        if (!(current_flags & MMU_AP_USER)) {
            spin_unlock_irqrestore(&process_table_lock, flags);
            return 0;
        }

        /* Check for write permissions (bit 7 is Read-Only, but COW allows it) */
        if (writable && (current_flags & (1ULL << 7)) && !(current_flags & MMU_PTE_COW)) {
            spin_unlock_irqrestore(&process_table_lock, flags);
            return 0;
        }

        curr += PAGE_SIZE;
    }
    spin_unlock_irqrestore(&process_table_lock, flags);

    return 1;
}

/*
 * syscall_handle - The primary entry point for EL0 synchronous exceptions.
 */
void syscall_handle(struct exception_trap_frame *tf)
{
    uint64_t syscall_nr = tf->x[8];
    struct task *curr = sched_get_current();
    uint32_t pid = curr->pid;

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

        case SYS_YIELD: {
            schedule();
            break;
        }

        case SYS_SLEEP: {
            unsigned long ms = tf->x[0];
            sched_sleep_ms(ms);
            break;
        }

        case SYS_OPEN: {
            const char *path = (const char *)(tf->x[0]);
            int flags = (int)(tf->x[1]);

            char *kpath = heap_malloc(VFS_MAX_PATH_LEN);
            long copied = strncpy_from_user(kpath, path, VFS_MAX_PATH_LEN);
            if (copied < 0 || copied >= VFS_MAX_PATH_LEN) {
                heap_free(kpath);
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
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

        case SYS_GETDENTS: {
            int fd = (int)(tf->x[0]);
            void *buf = (void *)(tf->x[1]);
            size_t count = (size_t)(tf->x[2]);

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

            char *kpath = heap_malloc(VFS_MAX_PATH_LEN);
            long copied = strncpy_from_user(kpath, path, VFS_MAX_PATH_LEN);
            if (copied < 0 || copied >= VFS_MAX_PATH_LEN) {
                heap_free(kpath);
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            int res = process_exec(kpath, argv, envp);
            heap_free(kpath);

            if (res < 0) {
                tf->x[0] = (uint64_t)res;
                break;
            }

            struct task *curr_task = sched_get_current();
            if (curr_task) {
                /* Synchronize trap frame after successful image replacement */
                uintptr_t kernel_stack_top =
                    process_table[curr_task->pid].vaddr_kernel_stack + SCHED_TASK_STACK_SIZE;
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

            int curr_proc_pid = process_find_current();
            if (curr_proc_pid < 0) {
                tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
                break;
            }

            struct process *p = &process_table[curr_proc_pid];

            signal_handler_t old = p->signal_handlers[sig - 1].sa_handler;
            p->signal_handlers[sig - 1].sa_handler = handler;
            p->signal_handlers[sig - 1].sa_mask = 0;
            p->signal_handlers[sig - 1].sa_flags = 0;
            p->signal_handlers[sig - 1].sa_restorer = NULL;

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

            if (target_pid == 0) {
                tf->x[0] = (uint64_t)-PERS_ERR_PERMISSION_DENIED;
                break;
            }

            if (target_pid >= PROCESS_TABLE_SIZE
                || process_table[target_pid].state == PROCESS_STATE_EMPTY) {
                tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
                break;
            }

            /* Enforce process hierarchy permissions */
            if (target_pid != (int)pid && process_table[target_pid].parent_pid != pid
                && (int)process_table[pid].parent_pid != target_pid) {
                tf->x[0] = (uint64_t)-PERS_ERR_PERMISSION_DENIED;
                break;
            }

            tf->x[0] = (uint64_t)signal_send(target_pid, sig);
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

            process_table[pid].blocked_signals = frame.saved_mask;
            tf->spsr_el1 &= 0xF0000000ULL;
            break;

sigreturn_kill:
            process_table[pid].exit_status = -1;
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
            process_table[pid].default_sigrestorer = restorer;
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

            struct process *p = &process_table[pid];

            if (uoact) {
                if (!validate_user_buffer(uoact, sizeof(struct sigaction), 1)) {
                    tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                    break;
                }
                if (copy_to_user(uoact, &p->signal_handlers[sig - 1], sizeof(struct sigaction))
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
                p->signal_handlers[sig - 1] = kact;
                p->signal_handlers[sig - 1].sa_mask &=
                    ~((1u << (SIGNAL_KILL - 1)) | (1u << (SIGNAL_STOP - 1)));
            }

            tf->x[0] = PERS_SUCCESS;
            break;
        }

        case SYS_SIGPROCMASK: {
            int how = (int)tf->x[0];
            const sigset_t *uset = (const sigset_t *)tf->x[1];
            sigset_t *uoset = (sigset_t *)tf->x[2];

            struct process *p = &process_table[pid];

            if (uoset) {
                if (!validate_user_buffer(uoset, sizeof(sigset_t), 1)) {
                    tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                    break;
                }
                if (copy_to_user(uoset, &p->blocked_signals, sizeof(sigset_t)) != 0) {
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
                    p->blocked_signals |= kset;
                } else if (how == SIG_UNBLOCK) {
                    p->blocked_signals &= ~kset;
                } else if (how == SIG_SETMASK) {
                    p->blocked_signals = kset;
                } else {
                    tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                    break;
                }

                p->blocked_signals &= ~((1u << (SIGNAL_KILL - 1)) | (1u << (SIGNAL_STOP - 1)));
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
            if (copy_to_user(uset, &process_table[pid].pending_signals, sizeof(sigset_t)) != 0) {
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

            struct process *p = &process_table[pid];
            p->blocked_signals = kmask;
            p->blocked_signals &= ~((1u << (SIGNAL_KILL - 1)) | (1u << (SIGNAL_STOP - 1)));

            while (!(p->pending_signals & ~p->blocked_signals)) {
                curr->state = SCHED_TASK_BLOCKED;
                schedule();
            }

            tf->x[0] = (uint64_t)-PERS_ERR_INTERRUPTED;
            break;
        }

        case SYS_CHDIR: {
            const char *path = (const char *)(tf->x[0]);
            if (!validate_user_buffer(path, 1, 0)) {
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
                break;
            }

            char *kpath = heap_malloc(VFS_MAX_PATH_LEN);
            if (!kpath) {
                tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }

            long copied = strncpy_from_user(kpath, path, VFS_MAX_PATH_LEN);
            if (copied < 0 || copied >= VFS_MAX_PATH_LEN) {
                heap_free(kpath);
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
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

            char *kpath = heap_malloc(VFS_MAX_PATH_LEN);
            if (strncpy_from_user(kpath, upath, VFS_MAX_PATH_LEN) < 0) {
                heap_free(kpath);
                tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
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

            int curr_proc_pid = process_find_current();
            if (curr_proc_pid < 0) {
                tf->x[0] = (uintptr_t)MAP_FAILED;
                break;
            }

            struct process *p = &process_table[curr_proc_pid];
            if (length == 0 || length > SYSCALL_MAX_MMAP_SIZE) {
                tf->x[0] = (uintptr_t)MAP_FAILED;
                break;
            }

            size_t pages_needed = (length + PAGE_SIZE - 1) / PAGE_SIZE;
            uintptr_t new_region = process_va_alloc(&p->va, pages_needed);
            if (new_region == 0) {
                tf->x[0] = (uintptr_t)MAP_FAILED;
                break;
            }

            if (fd != -1) {
                struct vfs_file *file = p->fd_table[fd];
                if (file == NULL) {
                    tf->x[0] = (uintptr_t)MAP_FAILED;
                    break;
                }
                if (file->node->ops->mmap != NULL) {
                    if (file->node->ops->mmap(file, new_region, length, prot, flags) < 0) {
                        tf->x[0] = (uintptr_t)MAP_FAILED;
                        break;
                    }
                }
            }

            if (flags & MAP_ANONYMOUS) {
                unsigned long mmu_flags = MMU_PTE_VALID | MMU_PTE_PAGE | MMU_PTE_AF
                                          | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_USER
                                          | MMU_PXN | MMU_UXN;
                if (!(prot & PROT_WRITE)) {
                    mmu_flags |= MMU_AP_RO;
                }

                for (size_t i = 0; i < pages_needed; i++) {
                    void *kaddr = pmm_alloc_page();
                    if (!kaddr) {
                        tf->x[0] = (uintptr_t)MAP_FAILED;
                        goto mmap_done;
                    }
                    memset(kaddr, 0, PAGE_SIZE);
                    mmu_user_map_page(p->user_pgd, new_region + i * PAGE_SIZE,
                                      V2P((uintptr_t)kaddr), mmu_flags);
                }
            }
            tf->x[0] = new_region;
mmap_done:
            break;
        }

        default: {
            pr_warn("syscall: unknown syscall: %lu\n", syscall_nr);
            break;
        }
    }
}
