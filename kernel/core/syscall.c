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
#include <stdint.h>

/*
 * validate_user_buffer - Verifies that a memory range provided by a user
 * process is valid, belongs to user-space, and has appropriate permissions.
 */
static int validate_user_buffer(const void* ptr, size_t len, int writable)
{
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
        process_table[pid].exit_status = status;
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

        size_t path_len = 0;
        const char* p = path;
        while (path_len < VFS_MAX_PATH_LEN)
        {
            unsigned char c;
            if (copy_from_user(&c, p++, 1) != 0)
            {
                path_len = (size_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }
            if (c == '\0')
            {
                break;
            }
            path_len++;
        }

        if (path_len == (size_t)-PERS_ERR_OUT_OF_MEMORY || path_len >= VFS_MAX_PATH_LEN)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
            break;
        }

        char* kpath = heap_malloc(path_len + 1);
        if (copy_from_user(kpath, path, path_len + 1) != 0)
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

        if (!validate_user_buffer(buf, len, 1))
        {
            tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
            break;
        }

        char* kbuf = heap_malloc(len);
        int bytes = vfs_read(fd, kbuf, len);
        if (bytes > 0)
        {
            if (copy_to_user(buf, kbuf, (size_t)bytes) != 0)
            {
                bytes = -PERS_ERR_INVALID_ARGUMENT;
            }
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
            tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
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
            if (copy_to_user(buf, kbuf, count) != 0)
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
        // /* TEMP DEBUG */
        // int cur_pid = process_find_current();
        // unsigned long dbg_paddr = 0, dbg_flags = 0;
        // int dbg_mapped =
        //     (cur_pid >= 0)
        //     && mmu_user_query(process_table[cur_pid].user_pgd, (unsigned long)path & ~0xFFFUL, &dbg_paddr,
        //     &dbg_flags);
        // printf("[EXEC DEBUG] path=0x%lx mapped=%d paddr=0x%lx flags=0x%lx\n",
        //        (unsigned long)path,
        //        dbg_mapped,
        //        dbg_paddr,
        //        dbg_flags);
        // if (dbg_mapped)
        // {
        //     const char* direct = (const char*)P2V(dbg_paddr) + ((unsigned long)path & 0xFFF);
        //     printf("[EXEC DEBUG] P2V direct read: '%.16s'\n", direct);
        // }

        size_t path_len = 0;
        const char* p = path;
        // unsigned long actual_ttbr0;
        // asm volatile("mrs %0, ttbr0_el1" : "=r"(actual_ttbr0));
        // printf("[EXEC DEBUG] actual TTBR0=0x%lx expected=0x%lx\n",
        //        actual_ttbr0,
        //        (unsigned long)process_table[cur_pid].ttbr0);
        // /* Read PGD[0] via software (kernel virtual address) */
        // unsigned long* pgd_virt = (unsigned long*)P2V(actual_ttbr0 & 0xFFFFFFFFFFFUL);
        // unsigned long pgd_entry_sw = pgd_virt[0];
        //
        // /* Force a cache flush of the PGD page to ensure PTW sees same data */
        // asm volatile("dc civac, %0" ::"r"(pgd_virt) : "memory");
        // asm volatile("dsb ish" ::: "memory");
        //
        // /* Read again after flush */
        // unsigned long pgd_entry_after = pgd_virt[0];
        //
        // printf("[EXEC DEBUG] PGD[0] sw=0x%lx after_flush=0x%lx\n", pgd_entry_sw, pgd_entry_after);
        //
        // /* Walk manually to L3 entry for 0x102060 */
        // if (pgd_entry_sw & 1)
        // {
        //     unsigned long* pmd = (unsigned long*)P2V(pgd_entry_sw & 0x0000FFFFFFFFF000ULL);
        //     unsigned long pmd_entry = pmd[0]; /* L2 index = 0 */
        //     printf("[EXEC DEBUG] PMD[0]=0x%lx\n", pmd_entry);
        //     if (pmd_entry & 1)
        //     {
        //         unsigned long* pte_table = (unsigned long*)P2V(pmd_entry & 0x0000FFFFFFFFF000ULL);
        //         unsigned long pte = pte_table[0x102]; /* L3 index for 0x102060 */
        //         printf("[EXEC DEBUG] PTE[0x102]=0x%lx\n", pte);
        //     }
        // }
        while (path_len < VFS_MAX_PATH_LEN)
        {
            unsigned char c;
            if (copy_from_user(&c, p++, 1) != 0)
            {
                path_len = (size_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }
            if (c == '\0')
            {
                break;
            }
            path_len++;
        }

        if (path_len == (size_t)-PERS_ERR_OUT_OF_MEMORY || path_len >= VFS_MAX_PATH_LEN)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
            break;
        }

        char* kpath = heap_malloc(path_len + 1);
        // unsigned char direct_read = *(volatile unsigned char*)(uintptr_t)path;
        // printf("[EXEC DEBUG] direct deref byte0=0x%02x (expect 0x2f for '/')\n", direct_read);
        if (copy_from_user(kpath, path, path_len + 1) != 0)
        {
            heap_free(kpath);
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }

        // printf("[EXEC DEBUG] syscall tf=0x%lx x0=0x%lx path_ptr=0x%lx\n",
        //        (unsigned long)tf,
        //        (unsigned long)tf->x[0],
        //        (unsigned long)(const char*)(tf->x[0]));
        int res = process_exec(kpath);
        //
        // printf("[EXEC DEBUG] process_exec('%s') = %d\n", kpath, res);  // ADD THIS
        heap_free(kpath);

        if (res < 0)
        {
            tf->x[0] = (uint64_t)res;
        }
        break;
    }

    case SYS_FORK:
    { /* sys_fork() */
        tf->x[0] = (uint64_t)process_fork(tf);
        break;
    }

    case SYS_WAITPID:
    { /* sys_waitpid(int pid, int* status) */
        int wait_pid = (int)tf->x[0];
        int* ustatus = (int*)tf->x[1];
        int kstatus = 0;

        int res = process_waitpid(wait_pid, &kstatus);
        if (res >= 0 && ustatus)
        {
            if (validate_user_buffer(ustatus, sizeof(int), 1))
            {
                if (copy_to_user(ustatus, &kstatus, sizeof(int)) != 0)
                {
                    tf->x[0] = -PERS_ERR_UNKNOWN;
                    break;
                }
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

        curr_process->signal_handlers[sig - 1] = handler;

        tf->x[0] = PERS_SUCCESS;
        break;
    }

    case SYS_KILL:
    { /* sys_kill(int pid, int sig) */
        int pid = (int)tf->x[0];
        int sig = (int)tf->x[1];

        if (sig >= SIGNAL_COUNT || sig < 1)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_INVALID_ARGUMENT;
            break;
        }
        if (pid < 0 || pid >= PROCESS_TABLE_SIZE || process_table[pid].state == PROCESS_STATE_EMPTY)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
            break;
        }

        process_table[pid].pending_signals |= (1 << (sig - 1));

        tf->x[0] = PERS_SUCCESS;

        break;
    }

    case SYS_SIGRETURN:
    {
        uintptr_t user_frame_ptr = tf->sp_el0;

        struct signal_frame frame;

        if (copy_from_user(&frame, (void*)user_frame_ptr, sizeof(struct signal_frame)) != 0)
        {
            process_exit(pid, -1);
            break;
        }

        memcpy(tf, &frame.saved_tf, sizeof(struct exception_trap_frame));

        break;
    }

    case SYS_SIGRESTORE:
    {
        uintptr_t restorer = (uintptr_t)tf->x[0];
        int curr_process_pid = process_find_current();
        struct process* curr_process = &process_table[curr_process_pid];
        curr_process->sig_restorer = restorer;
        tf->x[0] = PERS_SUCCESS;
        break;
    }

    case SYS_CHDIR:
    {
        const char* path = (const char*)(tf->x[0]);

        size_t path_len = 0;
        const char* p = path;
        while (path_len < VFS_MAX_PATH_LEN)
        {
            unsigned char c;
            if (copy_from_user(&c, p++, 1) != 0)
            {
                path_len = (size_t)-PERS_ERR_OUT_OF_MEMORY;
                break;
            }
            if (c == '\0')
            {
                break;
            }
            path_len++;
        }

        if (path_len == (size_t)-PERS_ERR_OUT_OF_MEMORY || path_len >= VFS_MAX_PATH_LEN)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
            break;
        }

        char* kpath = heap_malloc(path_len + 1);
        if (!kpath)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_OUT_OF_MEMORY;
            break;
        }
        if (copy_from_user(kpath, path, path_len + 1) != 0)
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
    case SYS_MMAP:
    {  // void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
        uintptr_t addr = (uintptr_t)tf->x[0];
        size_t length = (size_t)tf->x[1];
        int prot = (int)tf->x[2];
        int flags = (int)tf->x[3];
        int fd = (int)tf->x[4];
        off_t offset = (off_t)tf->x[5];

        int curr_process_pid = process_find_current();
        if (curr_process_pid < 0)
        {
            tf->x[0] = (uint64_t)-PERS_ERR_NO_SUCH_PROCESS;
            break;
        }

        struct process* curr_process = &process_table[curr_process_pid];

        size_t pages_needed = length / PAGE_SIZE;
        if (length > 0 && length % PAGE_SIZE != 0)
            pages_needed++;

        uintptr_t new_region = process_va_alloc(&curr_process->va, pages_needed);
        if (new_region == 0)
        {
            tf->x[0] = -PERS_ERR_OUT_OF_MEMORY;
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
            void* kaddr = pmm_alloc_pages(pages_needed);
            memset(kaddr, 0, pages_needed * PAGE_SIZE);
            for (int i = 0; i < pages_needed; i++)
            {
                uintptr_t phys = V2P((uintptr_t)kaddr + i * PAGE_SIZE);
                mmu_user_map_page(curr_process->user_pgd, new_region + i * PAGE_SIZE, phys, mmu_flags);
            }
        }
        tf->x[0] = new_region;
        break;
    }

    default:
    {
        printf("Unknown syscall: %lu\n", syscall_nr);
        break;
    }
    }
}
