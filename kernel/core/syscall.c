/*
 * syscall.c - System call dispatcher and userspace interface.
 */

#include "core/syscall.h"

#include "stdio.h"
#include "string.h"
#include "panic.h"

#include "uapi/mman.h"
#include "uapi/syscalls.h"
#include "uapi/errors.h"
#include "uapi/time.h"

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

int validate_user_buffer(const void *ptr, size_t len, int writable)
{
    if (!ptr || len == 0) {
        return 0;
    }

    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end = start + len;

    // Prevent wrap-around or kernel-space intrusion
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
 * copy_buf_from_user - Copies a user buffer into a fresh kernel buffer.
 *
 * Validates that the user range is readable and within size bounds.
 * On success the caller owns *out and must heap_free it.
 */
static int copy_buf_from_user(const void *ubuf, size_t len, void **out)
{
    *out = NULL;

    if (len == 0 || len > SYSCALL_MAX_RW_SIZE) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    if (!validate_user_buffer(ubuf, len, 0)) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    void *kbuf = heap_malloc(len);
    if (!kbuf) {
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    if (copy_from_user(kbuf, ubuf, len) != 0) {
        heap_free(kbuf);
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    *out = kbuf;
    return PERS_SUCCESS;
}

/*
 * alloc_user_out_buf - Prepares a kernel bounce buffer for writing to user memory.
 *
 * Validates that the destination user range is writable and within size bounds.
 * On success the caller owns *out and must heap_free it.
 */
static int alloc_user_out_buf(const void *ubuf, size_t len, void **out)
{
    *out = NULL;

    if (len == 0 || len > SYSCALL_MAX_RW_SIZE) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    if (!validate_user_buffer(ubuf, len, 1)) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    void *kbuf = heap_malloc(len);
    if (!kbuf) {
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    *out = kbuf;
    return PERS_SUCCESS;
}

/*
 * copy_buf_to_user - Validates and copies a kernel buffer to a user buffer.
 *
 * Used when the output length is only known after the kernel operation finishes.
 */
static int copy_buf_to_user(void *ubuf, const void *kbuf, size_t len)
{
    if (len == 0) {
        return PERS_SUCCESS;
    }

    if (!validate_user_buffer(ubuf, len, 1)) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    if (copy_to_user(ubuf, kbuf, len) != 0) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    return PERS_SUCCESS;
}

/*
 * Sentinel returned by syscall handlers that configure or overwrite the trap
 * frame themselves, signaling syscall_handle() not to write a return value
 * into tf->x[0].
 *
 * Used by:
 *   - sys_exec_handler: process_exec() builds the new frame and sets argc in
 *     tf->x[0]; storing a return value would destroy the argument count.
 *   - sys_sigreturn_handler: restores the user trap frame verbatim from the
 *     stack; storing a return value would corrupt the restored register state.
 *
 * Why INT64_MAX is collision-free across all other syscalls:
 *   - mmap returns user virtual addresses below USER_VA_LIMIT (0x8000000000)
 *     or MAP_FAILED (-1).
 *   - lseek returns file offsets (vfs_off_t, positive values up to file size).
 *   - All other handlers return small non-negative integers (byte counts, PIDs,
 *     file descriptors, 0 for success) or negative error codes (-PERS_ERR_*).
 * None can legitimately return INT64_MAX. Note: any future syscall returning an
 * unconstrained 64-bit value must take care not to collide with this sentinel.
 */
#define SYSCALL_RETAIN_FRAME ((int64_t)0x7FFFFFFFFFFFFFFFLL)

typedef int64_t (*syscall_fn)(struct exception_trap_frame *tf);

static int64_t sys_getpid_handler(struct exception_trap_frame *tf)
{
    (void)tf;
    return (int64_t)sched_get_current()->pid;
}

static int64_t sys_yield_handler(struct exception_trap_frame *tf)
{
    (void)tf;
    schedule();
    return PERS_SUCCESS;
}

static int64_t sys_getppid_handler(struct exception_trap_frame *tf)
{
    (void)tf;
    struct process *proc = process_current();
    if (!proc) {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }
    return (int64_t)proc->parent_pid;
}

static int64_t sys_setpgid_handler(struct exception_trap_frame *tf)
{
    int target_pid = (int)tf->x[0];
    int new_pgid = (int)tf->x[1];

    int curr_pid = process_find_current();
    if (curr_pid < 0) {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }

    if (target_pid == 0) {
        target_pid = curr_pid;
    }
    if (new_pgid == 0) {
        new_pgid = target_pid;
    }

    // A pgid is always some process's pid, so it carries the same bound.
    if (target_pid < 1 || target_pid >= PROCESS_TABLE_SIZE || new_pgid < 1
        || new_pgid >= PROCESS_TABLE_SIZE) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    unsigned long irqf = spin_lock_irqsave(&process_table_lock);
    struct process *target_proc = process_table[target_pid];
    if (!target_proc || target_proc->state != PROCESS_STATE_RUNNING) {
        spin_unlock_irqrestore(&process_table_lock, irqf);
        return -PERS_ERR_NO_SUCH_PROCESS;
    }

    // Restrict setpgid to self or direct child
    if (target_pid != curr_pid && (int)target_proc->parent_pid != curr_pid) {
        spin_unlock_irqrestore(&process_table_lock, irqf);
        return -PERS_ERR_PERMISSION_DENIED;
    }

    /* A parent may only place a child before it execs; the new image
     * owns its own membership afterwards. Moving yourself stays legal. */
    if (target_pid != curr_pid && target_proc->has_execed) {
        spin_unlock_irqrestore(&process_table_lock, irqf);
        return -PERS_ERR_PERMISSION_DENIED;
    }

    /* A session leader's pid names its session, so it cannot also name a
     * group elsewhere. */
    if (target_proc->sid == target_proc->pid) {
        spin_unlock_irqrestore(&process_table_lock, irqf);
        return -PERS_ERR_PERMISSION_DENIED;
    }

    /* Groups do not span sessions; an empty group is only legal when the
     * target is creating its own. */
    uint32_t group_sid = pgid_session_locked((uint32_t)new_pgid);
    if (group_sid == 0) {
        if (new_pgid != target_pid) {
            spin_unlock_irqrestore(&process_table_lock, irqf);
            return -PERS_ERR_PERMISSION_DENIED;
        }
    } else if (group_sid != target_proc->sid) {
        spin_unlock_irqrestore(&process_table_lock, irqf);
        return -PERS_ERR_PERMISSION_DENIED;
    }

    target_proc->pgid = (uint32_t)new_pgid;
    spin_unlock_irqrestore(&process_table_lock, irqf);
    return PERS_SUCCESS;
}

static int64_t sys_getpgid_handler(struct exception_trap_frame *tf)
{
    int target_pid = (int)tf->x[0];
    int curr_pid = process_find_current();
    if (curr_pid < 0) {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }
    if (target_pid == 0) {
        target_pid = curr_pid;
    }
    if (target_pid < 0 || target_pid >= PROCESS_TABLE_SIZE) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    unsigned long irqf = spin_lock_irqsave(&process_table_lock);
    struct process *target_proc = process_table[target_pid];
    if (!target_proc || target_proc->state != PROCESS_STATE_RUNNING) {
        spin_unlock_irqrestore(&process_table_lock, irqf);
        return -PERS_ERR_NO_SUCH_PROCESS;
    }
    uint32_t res_pgid = target_proc->pgid;
    spin_unlock_irqrestore(&process_table_lock, irqf);
    return (int64_t)res_pgid;
}

static int64_t sys_tcsetpgrp_handler(struct exception_trap_frame *tf)
{
    int fd = (int)tf->x[0];
    int new_pgid = (int)tf->x[1];

    int curr_pid = process_find_current();
    if (curr_pid < 0 || fd < 0 || fd >= VFS_MAX_FDS || new_pgid < 1
        || new_pgid >= PROCESS_TABLE_SIZE) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    struct process *curr_proc = process_slot((uint32_t)curr_pid);
    if (!curr_proc) {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }

    unsigned long fdflags = spin_lock_irqsave(&curr_proc->fd_lock);
    struct vfs_file *file = curr_proc->fd_table[fd];
    if (!file || !file->node || file->node->ops != &devfs_tty_ops) {
        spin_unlock_irqrestore(&curr_proc->fd_lock, fdflags);
        return -PERS_ERR_INVALID_ARGUMENT;
    }
    struct tty *tty = (struct tty *)file->node->internal_info;
    spin_unlock_irqrestore(&curr_proc->fd_lock, fdflags);

    if (!tty) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    unsigned long irqf = spin_lock_irqsave(&process_table_lock);
    uint32_t caller_sid = curr_proc->sid;
    int caller_is_leader = curr_proc->sid == curr_proc->pid;
    uint32_t group_sid = pgid_session_locked((uint32_t)new_pgid);
    spin_unlock_irqrestore(&process_table_lock, irqf);

    if (group_sid == 0 || group_sid != caller_sid) {
        return -PERS_ERR_PERMISSION_DENIED;
    }

    if (tty_access_check(tty, SIGNAL_TTOU) == TTY_ACCESS_STOPPED) {
        return -PERS_ERR_INTERRUPTED;
    }

    unsigned long ttyflags = spin_lock_irqsave(&tty->lock);
    if (tty->session_id == 0 && caller_is_leader) {
        tty->session_id = caller_sid;
    }
    if (tty->session_id != caller_sid) {
        spin_unlock_irqrestore(&tty->lock, ttyflags);
        return -PERS_ERR_PERMISSION_DENIED;
    }
    tty->foreground_pgid = (uint32_t)new_pgid;
    spin_unlock_irqrestore(&tty->lock, ttyflags);

    return PERS_SUCCESS;
}

static int64_t sys_tcgetpgrp_handler(struct exception_trap_frame *tf)
{
    int fd = (int)tf->x[0];
    int curr_pid = process_find_current();
    if (curr_pid < 0 || fd < 0 || fd >= VFS_MAX_FDS) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    struct process *curr_proc = process_slot((uint32_t)curr_pid);
    if (!curr_proc) {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }

    unsigned long fdflags = spin_lock_irqsave(&curr_proc->fd_lock);
    struct vfs_file *file = curr_proc->fd_table[fd];
    if (!file || !file->node || file->node->ops != &devfs_tty_ops) {
        spin_unlock_irqrestore(&curr_proc->fd_lock, fdflags);
        return -PERS_ERR_INVALID_ARGUMENT;
    }
    struct tty *tty = (struct tty *)file->node->internal_info;
    spin_unlock_irqrestore(&curr_proc->fd_lock, fdflags);

    if (!tty) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    unsigned long ttyflags = spin_lock_irqsave(&tty->lock);
    uint32_t fg_pgid = tty->foreground_pgid;
    spin_unlock_irqrestore(&tty->lock, ttyflags);

    return (int64_t)fg_pgid;
}

static int64_t sys_setsid_handler(struct exception_trap_frame *tf)
{
    (void)tf;
    int curr_pid = process_find_current();
    if (curr_pid < 0) {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }

    unsigned long irqf = spin_lock_irqsave(&process_table_lock);
    struct process *curr_p = process_table[curr_pid];
    if (!curr_p || curr_p->state != PROCESS_STATE_RUNNING) {
        spin_unlock_irqrestore(&process_table_lock, irqf);
        return -PERS_ERR_NO_SUCH_PROCESS;
    }

    if (curr_p->pgid == curr_p->pid) {
        spin_unlock_irqrestore(&process_table_lock, irqf);
        return -PERS_ERR_PERMISSION_DENIED;
    }

    curr_p->sid = curr_p->pid;
    curr_p->pgid = curr_p->pid;
    uint32_t new_sid = curr_p->sid;
    spin_unlock_irqrestore(&process_table_lock, irqf);

    return (int64_t)new_sid;
}

static int64_t sys_getsid_handler(struct exception_trap_frame *tf)
{
    int target_pid = (int)tf->x[0];
    int curr_pid = process_find_current();
    if (curr_pid < 0) {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }
    if (target_pid == 0) {
        target_pid = curr_pid;
    }
    if (target_pid < 0 || target_pid >= PROCESS_TABLE_SIZE) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    unsigned long irqf = spin_lock_irqsave(&process_table_lock);
    struct process *target_proc = process_table[target_pid];
    if (!target_proc || target_proc->state != PROCESS_STATE_RUNNING) {
        spin_unlock_irqrestore(&process_table_lock, irqf);
        return -PERS_ERR_NO_SUCH_PROCESS;
    }
    uint32_t res_sid = target_proc->sid;
    spin_unlock_irqrestore(&process_table_lock, irqf);
    return (int64_t)res_sid;
}

static int64_t sys_gettimeofday_handler(struct exception_trap_frame *tf)
{
    struct timeval *tv = (struct timeval *)tf->x[0];

    if (!tv || !validate_user_buffer(tv, sizeof(struct timeval), 1)) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    unsigned long ms = get_system_time();
    struct timeval ktv;
    ktv.tv_sec = (time_t)(ms / 1000);
    ktv.tv_usec = (long)((ms % 1000) * 1000);

    if (copy_to_user(tv, &ktv, sizeof(struct timeval)) != 0) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    return PERS_SUCCESS;
}

static int64_t sys_clock_gettime_handler(struct exception_trap_frame *tf)
{
    clockid_t clk_id = (clockid_t)tf->x[0];
    struct timespec *tp = (struct timespec *)tf->x[1];

    if (clk_id != CLOCK_REALTIME && clk_id != CLOCK_MONOTONIC) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    if (!tp || !validate_user_buffer(tp, sizeof(struct timespec), 1)) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    // TODO: no RTC — REALTIME is boot-relative, identical to MONOTONIC for now
    unsigned long ms = get_system_time();
    struct timespec ktp;
    ktp.tv_sec = (time_t)(ms / 1000);
    ktp.tv_nsec = (long)((ms % 1000) * 1000000);

    if (copy_to_user(tp, &ktp, sizeof(struct timespec)) != 0) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    return PERS_SUCCESS;
}

static int64_t sys_nanosleep_handler(struct exception_trap_frame *tf)
{
    const struct timespec *req = (const struct timespec *)tf->x[0];
    struct timespec *rem = (struct timespec *)tf->x[1];

    if (!req || !validate_user_buffer(req, sizeof(struct timespec), 0)) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    if (rem && !validate_user_buffer(rem, sizeof(struct timespec), 1)) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    struct timespec kreq;
    if (copy_from_user(&kreq, req, sizeof(struct timespec)) != 0) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    if (kreq.tv_sec < 0 || kreq.tv_nsec < 0 || kreq.tv_nsec > 999999999) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    /* Overflow guard: cap tv_sec to prevent (tv_sec * 1000) from overflowing unsigned long.
     */
    if (kreq.tv_sec > (time_t)(0x7FFFFFFFFFFFFFFFLL / 1000)) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    unsigned long ms =
        (unsigned long)kreq.tv_sec * 1000 + ((unsigned long)kreq.tv_nsec + 999999) / 1000000;

    if (ms > 0) {
        sched_sleep_ms(ms);
    }

    /* TODO: sched_sleep_ms is not signal-interruptible yet; sleep always completes, so rem
     * is always zero. Revisit when EINTR/signal-aware sleep lands. */
    if (rem) {
        struct timespec krem = {0, 0};
        if (copy_to_user(rem, &krem, sizeof(struct timespec)) != 0) {
            return -PERS_ERR_INVALID_ARGUMENT;
        }
    }

    return PERS_SUCCESS;
}

static int64_t sys_exit_handler(struct exception_trap_frame *tf)
{
    int status = (int)tf->x[0];
    struct task *curr = sched_get_current();
    process_exit(curr->pid, status);
}

static int64_t sys_fork_handler(struct exception_trap_frame *tf)
{
    return process_fork(tf);
}

static int64_t sys_waitpid_handler(struct exception_trap_frame *tf)
{
    int wait_pid = (int)tf->x[0];
    int *ustatus = (int *)tf->x[1];
    int options = (int)tf->x[2];
    int kstatus = 0;

    if (ustatus != NULL && !validate_user_buffer(ustatus, sizeof(int), 1)) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    int res = process_waitpid(wait_pid, &kstatus, options);
    if (res >= 0 && ustatus != NULL) {
        if (copy_to_user(ustatus, &kstatus, sizeof(int)) != 0) {
            return -PERS_ERR_OUT_OF_MEMORY;
        }
    }
    return res;
}

static int64_t sys_exec_handler(struct exception_trap_frame *tf)
{
    const char *path = (const char *)(tf->x[0]);
    char *const *argv = (char *const *)(tf->x[1]);
    char *const *envp = (char *const *)(tf->x[2]);

    if (!validate_user_buffer(path, 1, 0)) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    char *kpath;
    int err = copy_path_from_user(path, &kpath);
    if (err != PERS_SUCCESS) {
        return err;
    }

    int res = process_exec(kpath, argv, envp);
    heap_free(kpath);

    if (res < 0) {
        return res;
    }

    return SYSCALL_RETAIN_FRAME;
}

static int64_t sys_kill_handler(struct exception_trap_frame *tf)
{
    int target_pid = (int)tf->x[0];
    int sig = (int)tf->x[1];

    if (sig < 1 || sig >= SIGNAL_COUNT) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    struct task *curr = sched_get_current();
    uint32_t pid = curr->pid;
    struct process *proc = process_slot(pid);
    if (!proc) {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }

    /*
     * POSIX kill() pid rules:
     * target_pid > 0:  send to process target_pid.
     * target_pid == 0: send to caller's process group (proc->pgid).
     * target_pid == -1: reserved for broadcast; not supported yet, return
     * -PERS_ERR_NO_SUCH_PROCESS. target_pid < -1: send to process group (-target_pid).
     */
    if (target_pid == -1) {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }

    if (target_pid > 0) {
        if (target_pid >= PROCESS_TABLE_SIZE) {
            return -PERS_ERR_NO_SUCH_PROCESS;
        }

        struct process *target = process_slot((uint32_t)target_pid);
        if (!target || target->state != PROCESS_STATE_RUNNING) {
            return -PERS_ERR_NO_SUCH_PROCESS;
        }

        // Enforce process hierarchy permissions
        if (target_pid != (int)pid && target->parent_pid != pid
            && (int)proc->parent_pid != target_pid && target->pgid != proc->pgid) {
            return -PERS_ERR_PERMISSION_DENIED;
        }

        return signal_send((uint32_t)target_pid, sig);
    } else if (target_pid == 0) {
        if (proc->pgid == 0) {
            return -PERS_ERR_PERMISSION_DENIED;
        }
        return signal_send_group(proc->pgid, sig);
    } else { // target_pid < -1
        int64_t raw_pid = target_pid;
        uint64_t abs_pgid = (uint64_t)(-raw_pid);
        if (abs_pgid == 0 || abs_pgid >= PROCESS_TABLE_SIZE) {
            return -PERS_ERR_NO_SUCH_PROCESS;
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
                    if (p->parent_pid == proc->pid || (int)proc->parent_pid == (int)p->pid) {
                        allowed = 1;
                        break;
                    }
                }
            }
            spin_unlock_irqrestore(&process_table_lock, irqf);
        }

        if (!allowed) {
            return -PERS_ERR_PERMISSION_DENIED;
        }

        return signal_send_group(target_pgid, sig);
    }
}

static int64_t sys_sigaction_handler(struct exception_trap_frame *tf)
{
    int sig = (int)tf->x[0];
    const struct sigaction *uact = (const struct sigaction *)tf->x[1];
    struct sigaction *uoact = (struct sigaction *)tf->x[2];

    if (sig >= SIGNAL_COUNT || sig < 1 || sig == SIGNAL_KILL || sig == SIGNAL_STOP) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    struct task *curr = sched_get_current();
    struct process *proc = process_slot(curr->pid);
    if (!proc) {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }

    if (uoact) {
        if (!validate_user_buffer(uoact, sizeof(struct sigaction), 1)) {
            return -PERS_ERR_INVALID_ARGUMENT;
        }
        if (copy_to_user(uoact, &proc->signal_handlers[sig - 1], sizeof(struct sigaction)) != 0) {
            return -PERS_ERR_OUT_OF_MEMORY;
        }
    }

    if (uact) {
        if (!validate_user_buffer(uact, sizeof(struct sigaction), 0)) {
            return -PERS_ERR_INVALID_ARGUMENT;
        }
        struct sigaction kact;
        if (copy_from_user(&kact, uact, sizeof(struct sigaction)) != 0) {
            return -PERS_ERR_OUT_OF_MEMORY;
        }
        proc->signal_handlers[sig - 1] = kact;
        proc->signal_handlers[sig - 1].sa_mask &=
            ~((1u << (SIGNAL_KILL - 1)) | (1u << (SIGNAL_STOP - 1)));
    }

    return PERS_SUCCESS;
}

static int64_t sys_sigprocmask_handler(struct exception_trap_frame *tf)
{
    int how = (int)tf->x[0];
    const sigset_t *uset = (const sigset_t *)tf->x[1];
    sigset_t *uoset = (sigset_t *)tf->x[2];

    struct task *curr = sched_get_current();
    struct process *proc = process_slot(curr->pid);
    if (!proc) {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }

    if (uoset) {
        if (!validate_user_buffer(uoset, sizeof(sigset_t), 1)) {
            return -PERS_ERR_INVALID_ARGUMENT;
        }
        if (copy_to_user(uoset, &proc->blocked_signals, sizeof(sigset_t)) != 0) {
            return -PERS_ERR_OUT_OF_MEMORY;
        }
    }

    if (uset) {
        if (!validate_user_buffer(uset, sizeof(sigset_t), 0)) {
            return -PERS_ERR_INVALID_ARGUMENT;
        }
        sigset_t kset;
        if (copy_from_user(&kset, uset, sizeof(sigset_t)) != 0) {
            return -PERS_ERR_OUT_OF_MEMORY;
        }

        if (how == SIG_BLOCK) {
            proc->blocked_signals |= kset;
        } else if (how == SIG_UNBLOCK) {
            proc->blocked_signals &= ~kset;
        } else if (how == SIG_SETMASK) {
            proc->blocked_signals = kset;
        } else {
            return -PERS_ERR_INVALID_ARGUMENT;
        }

        proc->blocked_signals &= ~((1u << (SIGNAL_KILL - 1)) | (1u << (SIGNAL_STOP - 1)));
    }

    return PERS_SUCCESS;
}

static int64_t sys_sigpending_handler(struct exception_trap_frame *tf)
{
    sigset_t *uset = (sigset_t *)tf->x[0];
    if (!validate_user_buffer(uset, sizeof(sigset_t), 1)) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    struct task *curr = sched_get_current();
    struct process *proc = process_slot(curr->pid);
    if (!proc) {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }

    if (copy_to_user(uset, &proc->pending_signals, sizeof(sigset_t)) != 0) {
        return -PERS_ERR_OUT_OF_MEMORY;
    }
    return PERS_SUCCESS;
}

static int64_t sys_sigsuspend_handler(struct exception_trap_frame *tf)
{
    const sigset_t *umask = (const sigset_t *)tf->x[0];
    if (!validate_user_buffer(umask, sizeof(sigset_t), 0)) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }
    sigset_t kmask;
    if (copy_from_user(&kmask, umask, sizeof(sigset_t)) != 0) {
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    struct task *curr = sched_get_current();
    struct process *proc = process_slot(curr->pid);
    if (!proc) {
        return -PERS_ERR_NO_SUCH_PROCESS;
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

    // POSIX: sigsuspend restores the caller's original mask on return.
    proc->blocked_signals = saved_mask;
    return -PERS_ERR_INTERRUPTED;
}

static int64_t sys_sigreturn_handler(struct exception_trap_frame *tf)
{
    uintptr_t user_frame_ptr = tf->sp_el0;
    struct task *curr = sched_get_current();
    uint32_t pid = curr->pid;
    struct process *proc = process_slot(pid);

    if (user_frame_ptr == 0 || user_frame_ptr >= KERNEL_VMA
        || user_frame_ptr + sizeof(struct signal_frame) < user_frame_ptr) {
        process_exit(pid, -1);
    }

    if (!validate_user_buffer((void *)user_frame_ptr, sizeof(struct signal_frame), 0)) {
        process_exit(pid, -1);
    }

    struct signal_frame frame;
    if (copy_from_user(&frame, (void *)user_frame_ptr, sizeof(struct signal_frame)) != 0) {
        process_exit(pid, -1);
    }

    if (frame.saved_tf.elr_el1 >= KERNEL_VMA || frame.saved_tf.sp_el0 >= KERNEL_VMA) {
        process_exit(pid, -1);
    }

    memcpy(tf, &frame.saved_tf, sizeof(struct exception_trap_frame));

    if (proc) {
        proc->blocked_signals = frame.saved_mask;
    }
    tf->spsr_el1 &= 0xF0000000ULL;

    return SYSCALL_RETAIN_FRAME;
}

static int64_t sys_open_handler(struct exception_trap_frame *tf)
{
    const char *path = (const char *)(tf->x[0]);
    int flags = (int)(tf->x[1]);

    char *kpath;
    int err = copy_path_from_user(path, &kpath);
    if (err != PERS_SUCCESS) {
        return err;
    }

    int fd = vfs_open(kpath, flags);
    heap_free(kpath);
    return fd;
}

static int64_t sys_close_handler(struct exception_trap_frame *tf)
{
    int fd = (int)(tf->x[0]);
    return vfs_close(fd);
}

static int64_t sys_dup2_handler(struct exception_trap_frame *tf)
{
    int oldfd = (int)tf->x[0];
    int newfd = (int)tf->x[1];
    return vfs_dup2(oldfd, newfd);
}

static int64_t sys_pipe_handler(struct exception_trap_frame *tf)
{
    int *upipefd = (int *)tf->x[0];
    int kpipefd[2];

    if (!validate_user_buffer(upipefd, sizeof(int) * 2, 1)) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    int res = pipe_create(kpipefd);
    if (res == PERS_SUCCESS) {
        if (copy_to_user(upipefd, kpipefd, sizeof(int) * 2) != 0) {
            return -PERS_ERR_INVALID_ARGUMENT;
        }
    }
    return res;
}

static int64_t sys_lseek_handler(struct exception_trap_frame *tf)
{
    int fd = (int)tf->x[0];
    off_t offset = (off_t)tf->x[1];
    int whence = (int)tf->x[2];
    return vfs_lseek(fd, offset, whence);
}

static int64_t sys_fcntl_handler(struct exception_trap_frame *tf)
{
    int fd = (int)tf->x[0];
    int cmd = (int)tf->x[1];
    int arg = (int)tf->x[2];

    if (fd < 0 || fd >= VFS_MAX_FDS) {
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    struct task *curr = sched_get_current();
    struct process *proc = process_slot(curr->pid);
    if (!proc) {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }

    unsigned long fdflags = spin_lock_irqsave(&proc->fd_lock);
    struct vfs_file *f = proc->fd_table[fd];
    if (!f) {
        spin_unlock_irqrestore(&proc->fd_lock, fdflags);
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
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

    return ret;
}

static int64_t sys_chdir_handler(struct exception_trap_frame *tf)
{
    const char *path = (const char *)(tf->x[0]);
    if (!validate_user_buffer(path, 1, 0)) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    char *kpath;
    int err = copy_path_from_user(path, &kpath);
    if (err != PERS_SUCCESS) {
        return err;
    }

    int res = vfs_chdir(kpath);
    heap_free(kpath);
    return res;
}

static int64_t sys_stat_handler(struct exception_trap_frame *tf)
{
    const char *upath = (const char *)tf->x[0];
    struct stat *ubuf = (struct stat *)tf->x[1];

    if (!validate_user_buffer(upath, 1, 0) || !validate_user_buffer(ubuf, sizeof(struct stat), 1)) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    char *kpath;
    int err = copy_path_from_user(upath, &kpath);
    if (err != PERS_SUCCESS) {
        return err;
    }

    struct stat kbuf;
    int res = vfs_stat(kpath, &kbuf);
    heap_free(kpath);

    if (res == PERS_SUCCESS) {
        if (copy_to_user(ubuf, &kbuf, sizeof(struct stat)) != 0) {
            return -PERS_ERR_OUT_OF_MEMORY;
        }
    }
    return res;
}

static int64_t sys_sync_handler(struct exception_trap_frame *tf)
{
    (void)tf;
    pagecache_sync();
    block_cache_sync();
    return PERS_SUCCESS;
}

static int64_t sys_mkdir_handler(struct exception_trap_frame *tf)
{
    const char *path = (const char *)tf->x[0];
    if (!validate_user_buffer(path, 1, 0)) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }
    char *kpath;
    int err = copy_path_from_user(path, &kpath);
    if (err != PERS_SUCCESS) {
        return err;
    }
    int res = vfs_mkdir(kpath);
    heap_free(kpath);
    return res;
}

static int64_t sys_rmdir_handler(struct exception_trap_frame *tf)
{
    const char *path = (const char *)tf->x[0];
    if (!validate_user_buffer(path, 1, 0)) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }
    char *kpath;
    int err = copy_path_from_user(path, &kpath);
    if (err != PERS_SUCCESS) {
        return err;
    }
    int res = vfs_rmdir(kpath);
    heap_free(kpath);
    return res;
}

static int64_t sys_unlink_handler(struct exception_trap_frame *tf)
{
    const char *path = (const char *)tf->x[0];
    if (!validate_user_buffer(path, 1, 0)) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }
    char *kpath;
    int err = copy_path_from_user(path, &kpath);
    if (err != PERS_SUCCESS) {
        return err;
    }
    int res = vfs_unlink(kpath);
    heap_free(kpath);
    return res;
}

static int64_t sys_rename_handler(struct exception_trap_frame *tf)
{
    const char *oldpath = (const char *)tf->x[0];
    const char *newpath = (const char *)tf->x[1];
    if (!validate_user_buffer(oldpath, 1, 0) || !validate_user_buffer(newpath, 1, 0)) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }
    char *koldpath;
    int err = copy_path_from_user(oldpath, &koldpath);
    if (err != PERS_SUCCESS) {
        return err;
    }

    char *knewpath;
    err = copy_path_from_user(newpath, &knewpath);
    if (err != PERS_SUCCESS) {
        heap_free(koldpath);
        return err;
    }
    int res = vfs_rename(koldpath, knewpath);
    heap_free(koldpath);
    heap_free(knewpath);
    return res;
}

static int64_t sys_fsync_handler(struct exception_trap_frame *tf)
{
    int fd = (int)tf->x[0];
    return vfs_fsync(fd);
}

static int64_t sys_fstat_handler(struct exception_trap_frame *tf)
{
    int fd = (int)tf->x[0];
    struct stat *ubuf = (struct stat *)tf->x[1];

    if (!validate_user_buffer(ubuf, sizeof(struct stat), 1)) { // writable
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    struct stat kbuf;
    int res = vfs_fstat(fd, &kbuf);
    if (res == PERS_SUCCESS) {
        if (copy_to_user(ubuf, &kbuf, sizeof(struct stat)) != 0) {
            return -PERS_ERR_OUT_OF_MEMORY;
        }
    }
    return res;
}

static int64_t sys_truncate_handler(struct exception_trap_frame *tf)
{
    const char *upath = (const char *)tf->x[0];
    vfs_off_t length = (vfs_off_t)tf->x[1];

    char *kpath = NULL;
    int err = copy_path_from_user(upath, &kpath);
    if (err != PERS_SUCCESS) {
        return err;
    }

    int res = vfs_truncate(kpath, length);
    heap_free(kpath);
    return res;
}

static int64_t sys_ftruncate_handler(struct exception_trap_frame *tf)
{
    int fd = (int)tf->x[0];
    vfs_off_t length = (vfs_off_t)tf->x[1];
    return vfs_ftruncate(fd, length);
}

static int64_t sys_write_handler(struct exception_trap_frame *tf)
{
    int fd = (int)(tf->x[0]);
    const char *buf = (const char *)(tf->x[1]);
    size_t len = (size_t)(tf->x[2]);

    void *kbuf;
    int err = copy_buf_from_user(buf, len, &kbuf);
    if (err != PERS_SUCCESS) {
        return err;
    }

    int bytes = vfs_write(fd, kbuf, len);
    heap_free(kbuf);
    return bytes;
}

static int64_t sys_pwrite_handler(struct exception_trap_frame *tf)
{
    int fd = (int)(tf->x[0]);
    const char *buf = (const char *)(tf->x[1]);
    size_t len = (size_t)(tf->x[2]);
    vfs_off_t offset = (vfs_off_t)(tf->x[3]);

    void *kbuf;
    int err = copy_buf_from_user(buf, len, &kbuf);
    if (err != PERS_SUCCESS) {
        return err;
    }

    int bytes = vfs_pwrite(fd, kbuf, len, offset);
    heap_free(kbuf);
    return bytes;
}

static int64_t sys_read_handler(struct exception_trap_frame *tf)
{
    int fd = (int)(tf->x[0]);
    void *buf = (void *)(tf->x[1]);
    size_t len = (size_t)(tf->x[2]);

    void *kbuf;
    int err = alloc_user_out_buf(buf, len, &kbuf);
    if (err != PERS_SUCCESS) {
        return err;
    }

    int bytes = vfs_read(fd, kbuf, len);
    if (bytes > 0) {
        if (copy_to_user(buf, kbuf, (size_t)bytes) != 0) {
            bytes = -PERS_ERR_INVALID_ARGUMENT;
        }
    }
    heap_free(kbuf);
    return bytes;
}

static int64_t sys_pread_handler(struct exception_trap_frame *tf)
{
    int fd = (int)(tf->x[0]);
    void *buf = (void *)(tf->x[1]);
    size_t len = (size_t)(tf->x[2]);
    vfs_off_t offset = (vfs_off_t)(tf->x[3]);

    void *kbuf;
    int err = alloc_user_out_buf(buf, len, &kbuf);
    if (err != PERS_SUCCESS) {
        return err;
    }

    int bytes = vfs_pread(fd, kbuf, len, offset);
    if (bytes > 0) {
        if (copy_to_user(buf, kbuf, (size_t)bytes) != 0) {
            bytes = -PERS_ERR_INVALID_ARGUMENT;
        }
    }
    heap_free(kbuf);
    return bytes;
}

static int64_t sys_getdents_handler(struct exception_trap_frame *tf)
{
    int fd = (int)(tf->x[0]);
    void *buf = (void *)(tf->x[1]);
    size_t count = (size_t)(tf->x[2]);

    void *kbuf;
    int err = alloc_user_out_buf(buf, count, &kbuf);
    if (err != PERS_SUCCESS) {
        return err;
    }

    int res = vfs_readdir(fd, kbuf, count);
    if (res > 0) {
        size_t copy_size = (size_t)res * sizeof(struct vfs_dirent);
        if (copy_to_user(buf, kbuf, copy_size) != 0) {
            res = -PERS_ERR_INVALID_ARGUMENT;
        }
    }
    heap_free(kbuf);
    return res;
}

static int64_t sys_getcwd_handler(struct exception_trap_frame *tf)
{
    char *buf = (char *)tf->x[0];
    size_t size = (size_t)tf->x[1];

    if (!buf || size == 0) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    char *kbuf = heap_malloc(VFS_MAX_PATH_LEN);
    if (!kbuf) {
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    int res = vfs_getcwd(kbuf, VFS_MAX_PATH_LEN);
    if (res == PERS_SUCCESS) {
        size_t len = strlen(kbuf) + 1;
        if (len > size) {
            res = -PERS_ERR_INVALID_ARGUMENT;
        } else {
            int err = copy_buf_to_user(buf, kbuf, len);
            if (err != PERS_SUCCESS) {
                res = err;
            }
        }
    }

    heap_free(kbuf);
    return res;
}

static int64_t sys_mmap_handler(struct exception_trap_frame *tf)
{
    size_t length = (size_t)tf->x[1];
    int prot = (int)tf->x[2];
    int flags = (int)tf->x[3];
    int fd = (int)tf->x[4];

    if (length == 0 || length > SYSCALL_MAX_MMAP_SIZE) {
        return (int64_t)(uintptr_t)MAP_FAILED;
    }

    struct task *curr = sched_get_current();
    struct process *proc = process_slot(curr->pid);
    if (!proc) {
        return (int64_t)(uintptr_t)MAP_FAILED;
    }

    /*
     * Every mapping must have something behind it. An anonymous request
     * takes no descriptor; a file-backed one needs a vnode that can
     * actually supply pages. With neither, the address handed back is
     * one the caller can only discover is empty by faulting on it.
     */
    int anonymous = (flags & MAP_ANONYMOUS) != 0;
    if (anonymous ? (fd != -1) : (fd < 0 || fd >= VFS_MAX_FDS)) {
        return (int64_t)(uintptr_t)MAP_FAILED;
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
            return (int64_t)(uintptr_t)MAP_FAILED;
        }
    }

    size_t pages_needed = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t new_region = process_va_alloc(&proc->va, pages_needed);
    if (new_region == 0) {
        vfs_file_put(file);
        return (int64_t)(uintptr_t)MAP_FAILED;
    }

    if (!anonymous) {
        int mres = file->node->ops->mmap(file, new_region, length, prot, flags);
        vfs_file_put(file);
        if (mres < 0) {
            goto mmap_fail;
        }
    }

    if (anonymous) {
        unsigned long mmu_flags = MMU_PTE_VALID | MMU_PTE_PAGE | MMU_PTE_AF | MMU_PTE_SH_INNER
                                  | MMU_ATTR_NORMAL | MMU_AP_USER | MMU_PXN | MMU_UXN | MMU_PTE_NG;
        if (!(prot & PROT_WRITE)) {
            mmu_flags |= MMU_AP_RO;
        }

        for (size_t i = 0; i < pages_needed; i++) {
            void *kaddr = pmm_alloc_page();
            if (!kaddr) {
                goto mmap_fail;
            }
            memset(kaddr, 0, PAGE_SIZE);
            mmu_user_map_page(proc->user_pgd, new_region + i * PAGE_SIZE, V2P((uintptr_t)kaddr),
                              mmu_flags);
        }
    }
    return (int64_t)new_region;

mmap_fail:
    /* Unmap anything mapped so far (frees those pages) and release the
     * reserved VA region so a failed mmap leaks neither. */
    for (size_t j = 0; j < pages_needed; j++) {
        mmu_user_unmap_page(proc->user_pgd, new_region + j * PAGE_SIZE);
    }
    process_va_free(&proc->va, new_region);
    return (int64_t)(uintptr_t)MAP_FAILED;
}

static const syscall_fn syscall_table[SYS_FTRUNCATE + 1] = {
    [SYS_OPEN] = sys_open_handler,
    [SYS_WRITE] = sys_write_handler,
    [SYS_EXIT] = sys_exit_handler,
    [SYS_GETPID] = sys_getpid_handler,
    [SYS_YIELD] = sys_yield_handler,
    [SYS_READ] = sys_read_handler,
    [SYS_CLOSE] = sys_close_handler,
    [SYS_EXEC] = sys_exec_handler,
    [SYS_FORK] = sys_fork_handler,
    [SYS_WAITPID] = sys_waitpid_handler,
    [SYS_PIPE] = sys_pipe_handler,
    [SYS_DUP2] = sys_dup2_handler,
    [SYS_SIGRETURN] = sys_sigreturn_handler,
    [SYS_KILL] = sys_kill_handler,
    [SYS_GETDENTS] = sys_getdents_handler,
    [SYS_CHDIR] = sys_chdir_handler,
    [SYS_GETCWD] = sys_getcwd_handler,
    [SYS_MMAP] = sys_mmap_handler,
    [SYS_SIGACTION] = sys_sigaction_handler,
    [SYS_SIGPROCMASK] = sys_sigprocmask_handler,
    [SYS_SIGPENDING] = sys_sigpending_handler,
    [SYS_SIGSUSPEND] = sys_sigsuspend_handler,
    [SYS_STAT] = sys_stat_handler,
    [SYS_LSEEK] = sys_lseek_handler,
    [SYS_SYNC] = sys_sync_handler,
    [SYS_MKDIR] = sys_mkdir_handler,
    [SYS_RMDIR] = sys_rmdir_handler,
    [SYS_UNLINK] = sys_unlink_handler,
    [SYS_RENAME] = sys_rename_handler,
    [SYS_FSYNC] = sys_fsync_handler,
    [SYS_FCNTL] = sys_fcntl_handler,
    [SYS_PREAD] = sys_pread_handler,
    [SYS_PWRITE] = sys_pwrite_handler,
    [SYS_GETPPID] = sys_getppid_handler,
    [SYS_SETPGID] = sys_setpgid_handler,
    [SYS_GETPGID] = sys_getpgid_handler,
    [SYS_TCSETPGRP] = sys_tcsetpgrp_handler,
    [SYS_TCGETPGRP] = sys_tcgetpgrp_handler,
    [SYS_SETSID] = sys_setsid_handler,
    [SYS_GETSID] = sys_getsid_handler,
    [SYS_GETTIMEOFDAY] = sys_gettimeofday_handler,
    [SYS_CLOCK_GETTIME] = sys_clock_gettime_handler,
    [SYS_NANOSLEEP] = sys_nanosleep_handler,
    [SYS_FSTAT] = sys_fstat_handler,
    [SYS_TRUNCATE] = sys_truncate_handler,
    [SYS_FTRUNCATE] = sys_ftruncate_handler,
};

void syscall_handle(struct exception_trap_frame *tf)
{
    uint64_t syscall_nr = tf->x[8];
    struct task *curr = sched_get_current();

    /* A task running a syscall always has a live slot; refuse rather than
     * fault at EL1 if that ever stops holding. */
    if (!process_slot(curr->pid)) {
        tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
        return;
    }

    if (syscall_nr < sizeof(syscall_table) / sizeof(syscall_table[0])
        && syscall_table[syscall_nr] != NULL) {
        int64_t ret = syscall_table[syscall_nr](tf);
        if (ret != SYSCALL_RETAIN_FRAME) {
            tf->x[0] = (uint64_t)ret;
        }
        return;
    }

    pr_warn("syscall: unknown syscall: %lu\n", syscall_nr);
    tf->x[0] = (uint64_t)-PERS_ERR_NOT_IMPLEMENTED;
}
