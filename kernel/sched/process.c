/*
 * kernel/sched/process.c
 *
 * Core process management, lifecycle, and task definitions.
 * Handles user-space execution, process tracking, and address spaces.
 */

#include "sched/process.h"

#include "uapi/errors.h"

#include "arch/exception.h"

#include "mm/addr.h"
#include "core/elf.h"
#include "mm/mmu.h"
#include "panic.h"
#include "mm/pmm.h"
#include "sched/sched.h"
#include "mm/slab.h"
#include "stdio.h"
#include "string.h"
#include "core/timer.h"
#include "types.h"
#include "fs/vfs.h"

/* --- Global Variables --- */
spinlock_t process_table_lock = SPINLOCK_INIT;
struct process process_table[PROCESS_TABLE_SIZE];

/* --- External References --- */
extern void ret_to_user(void);

/* --- Private Helper Functions --- */

/*
 * Initializes the virtual address allocator for a new process.
 */
static void va_init(struct va_allocator* va)
{
    va->count = 0;
    va->next_va = USER_VA_BASE;
    for (size_t i = 0; i < USER_VA_MAX_REGIONS; i++)
    {
        va->regions[i].base = 0;
        va->regions[i].pages = 0;
    }
}

/*
 * Allocates a kernel stack for a new process.
 * Includes a guard page and a canary value.
 */
static void* alloc_kernel_stack(void)
{
    unsigned char* base = (unsigned char*)pmm_alloc_pages(SCHED_STACK_PAGES);
    if (!base)
    {
        return NULL;
    }

    mmu_unmap_page((unsigned long)base);

    *(unsigned long*)(base + PAGE_SIZE) = SCHED_STACK_CANARY;

    return base + PAGE_SIZE;
}

/* --- Public API Implementations --- */

/*
 * Flushes the instruction cache for a given range.
 */
void process_flush_icache_range(void* start, size_t size)
{
    unsigned long addr = (unsigned long)start & ~63UL;
    unsigned long end = ((unsigned long)start + size + 63UL) & ~63UL;

    for (unsigned long a = addr; a < end; a += 64)
    {
        asm volatile("dc cvau, %0" ::"r"(a) : "memory");
    }
    asm volatile("dsb ish" ::: "memory");

    for (unsigned long a = addr; a < end; a += 64)
    {
        asm volatile("ic ivau, %0" ::"r"(a) : "memory");
    }
    asm volatile("dsb ish" ::: "memory");
    asm volatile("isb");
}

/*
 * Allocates user virtual memory regions.
 */
uintptr_t process_va_alloc(struct va_allocator* va, size_t pages)
{
    if (pages == 0 || va->count >= USER_VA_MAX_REGIONS)
    {
        return 0;
    }

    uintptr_t base = va->next_va;
    size_t size = pages * PAGE_SIZE;

    if (base + size < base)
    {  // overflow
        return 0;
    }
    if (base + size > 0x8000000000ULL)
    {  // 39-bit VA limit
        return 0;
    }

    va->next_va = base + size;
    va->regions[va->count].base = base;
    va->regions[va->count].pages = pages;
    va->count++;
    return base;
}

/*
 * Frees user virtual memory regions.
 */
void process_va_free(struct va_allocator* va, uintptr_t base)
{
    for (size_t i = 0; i < va->count; i++)
    {
        if (va->regions[i].base == base)
        {
            for (size_t j = i; j + 1 < va->count; j++)
            {
                va->regions[j] = va->regions[j + 1];
            }
            va->count--;
            return;
        }
    }
}

/*
 * Returns the PID of the currently running process.
 */
int process_find_current(void)
{
    struct task* t = sched_get_current();
    if (!t)
    {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }

    return (int)t->pid;
}

/*
 * Initializes the process subsystem.
 */
void process_init(void)
{
    memset(process_table, 0, sizeof(process_table));
    for (size_t i = 0; i < PROCESS_TABLE_SIZE; i++)
    {
        process_table[i].state = PROCESS_STATE_EMPTY;
        process_table[i].fd_lock = (spinlock_t)SPINLOCK_INIT;
    }

    // PID 0 is the kernel/boot process
    process_table[0].pid = 0;
    process_table[0].state = PROCESS_STATE_RUNNING;
    process_table[0].user_pgd = NULL;
    process_table[0].asid = 0;

    printf("[ PROC ] Process management initialized (%zu slots)\n", (size_t)PROCESS_TABLE_SIZE);
}

/*
 * Manually creates a user process from a raw code blob (used for initial user tasks).
 */
void process_create(void* code_ptr, size_t code_size, uint32_t pid)
{
    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    if (pid >= PROCESS_TABLE_SIZE)
    {
        spin_unlock_irqrestore(&process_table_lock, flags);
        printf("[PROCESS] Error: PID %d out of range\n", pid);
        return;
    }
    if (process_table[pid].state != PROCESS_STATE_EMPTY)
    {
        spin_unlock_irqrestore(&process_table_lock, flags);
        printf("[PROCESS] Error: PID %d already in use\n", pid);
        return;
    }
    process_table[pid].state = PROCESS_STATE_RUNNING;
    spin_unlock_irqrestore(&process_table_lock, flags);

    va_init(&process_table[pid].va);

    if (code_size == 0 || code_size > PAGE_SIZE)
    {
        printf("[PROCESS] Error: code_size %zu invalid for single-page alloc\n", code_size);
        return;
    }

    void* code_page = pmm_alloc_page();
    if (!code_page)
    {
        PANIC("Out of memory for process code");
    }

    process_table[pid].vaddr_code = process_va_alloc(&process_table[pid].va, 1);
    if (!process_table[pid].vaddr_code)
    {
        PANIC("Out of virtual address space for process code");
    }
    process_table[pid].vaddr_user_stack = process_va_alloc(&process_table[pid].va, PROCESS_USER_STACK_PAGES);
    if (!process_table[pid].vaddr_user_stack)
    {
        PANIC("Out of virtual address space for process stack");
    }

    process_table[pid].paddr_code = V2P(code_page);

    unsigned long* user_pgd = mmu_create_user_pgd();
    if (!user_pgd)
    {
        PANIC("Failed to create user PGD for process");
    }

    process_table[pid].user_pgd = user_pgd;
    process_table[pid].asid = pid;
    process_table[pid].ttbr0 = V2P(user_pgd) | ((uint64_t)pid << 48);

    mmu_user_map_page(user_pgd, process_table[pid].vaddr_code, process_table[pid].paddr_code, MMU_PAGE_USER_CODE);
    for (size_t i = 0; i < PROCESS_USER_STACK_PAGES; i++)
    {
        void* spage = pmm_alloc_page();
        if (!spage)
        {
            PANIC("Out of memory for process user stack page");
        }
        mmu_user_map_page(
            user_pgd, process_table[pid].vaddr_user_stack + i * PAGE_SIZE, V2P(spage), MMU_PAGE_USER_DATA);
    }

    void* kstack = alloc_kernel_stack();
    process_table[pid].vaddr_kernel_stack = (uintptr_t)kstack;
    process_table[pid].paddr_kernel_stack = V2P(kstack);

    memcpy(code_page, code_ptr, code_size);
    process_flush_icache_range(code_page, code_size);

    asm volatile("ic ialluis\n dsb ish\n isb");

    uintptr_t kernel_stack_top = process_table[pid].vaddr_kernel_stack + SCHED_TASK_STACK_SIZE;
    struct exception_trap_frame* tf =
        (struct exception_trap_frame*)(kernel_stack_top - sizeof(struct exception_trap_frame));
    memset(tf, 0, sizeof(struct exception_trap_frame));
    tf->elr_el1 = process_table[pid].vaddr_code;
    tf->spsr_el1 = SPSR_EL0_USER;
    tf->sp_el0 = (process_table[pid].vaddr_user_stack + PROCESS_USER_STACK_PAGES * PAGE_SIZE) & ~15UL;

    process_table[pid].context.sp = (unsigned long)tf;
    process_table[pid].context.lr = (unsigned long)ret_to_user;

    process_table[pid].pid = pid;
    process_table[pid].state = PROCESS_STATE_RUNNING;
    int err;
    process_table[pid].cwd = vfs_resolve_path("/", NULL, &err);

    for (size_t i = 0; i < VFS_MAX_FDS; i++)
    {
        process_table[pid].fd_table[i] = NULL;
    }
    vfs_open_pid("/dev/uart", VFS_O_RDONLY, pid);
    vfs_open_pid("/dev/uart", VFS_O_WRONLY, pid);
    vfs_open_pid("/dev/uart", VFS_O_WRONLY, pid);

    struct task* t =
        sched_create_user_task(process_table[pid].context.sp, process_table[pid].context.lr, process_table[pid].pid);
    process_table[pid].main_task = t;
    enqueue_ready(get_core_id(), t);
}

/*
 * Creates a process from an ELF file on disk.
 */
int process_create_from_file(const char* path, uint32_t pid)
{
    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    if (pid >= PROCESS_TABLE_SIZE)
    {
        spin_unlock_irqrestore(&process_table_lock, flags);
        return -PERS_ERR_INVALID_ARGUMENT;
    }
    if (process_table[pid].state != PROCESS_STATE_EMPTY)
    {
        spin_unlock_irqrestore(&process_table_lock, flags);
        return -PERS_ERR_ALREADY_EXISTS;
    }
    process_table[pid].state = PROCESS_STATE_RUNNING;
    spin_unlock_irqrestore(&process_table_lock, flags);

    va_init(&process_table[pid].va);

    unsigned long* user_pgd = mmu_create_user_pgd();
    if (!user_pgd)
    {
        process_table[pid].state = PROCESS_STATE_EMPTY;
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    uint64_t entry_point;
    if (elf_load(path, user_pgd, &entry_point) != 0)
    {
        mmu_destroy_user_pgd(user_pgd);
        process_table[pid].state = PROCESS_STATE_EMPTY;
        return -PERS_ERR_EXECUTABLE_FORMAT_ERROR;
    }

    size_t stack_pages = 32;
    uintptr_t vaddr_stack = process_va_alloc(&process_table[pid].va, stack_pages);
    if (!vaddr_stack)
    {
        mmu_destroy_user_pgd(user_pgd);
        process_table[pid].state = PROCESS_STATE_EMPTY;
        return -PERS_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < stack_pages; i++)
    {
        void* page = pmm_alloc_page();
        if (!page)
        {
            PANIC("Out of memory for user stack");
        }
        mmu_user_map_page(user_pgd, vaddr_stack + i * PAGE_SIZE, V2P(page), MMU_PAGE_USER_DATA);
    }

    void* kstack = alloc_kernel_stack();
    if (!kstack)
    {
        PANIC("Out of memory for kernel stack");
    }

    process_table[pid].pid = pid;
    process_table[pid].state = PROCESS_STATE_RUNNING;
    process_table[pid].user_pgd = user_pgd;
    process_table[pid].asid = pid;
    process_table[pid].ttbr0 = V2P(user_pgd) | ((uint64_t)pid << 48);
    process_table[pid].vaddr_code = entry_point;
    process_table[pid].vaddr_user_stack = vaddr_stack;
    process_table[pid].vaddr_kernel_stack = (uintptr_t)kstack;
    process_table[pid].paddr_kernel_stack = V2P(kstack);

    int err;
    process_table[pid].cwd = vfs_resolve_path("/", NULL, &err);

    uintptr_t kernel_stack_top = (uintptr_t)kstack + SCHED_TASK_STACK_SIZE;
    struct exception_trap_frame* tf =
        (struct exception_trap_frame*)(kernel_stack_top - sizeof(struct exception_trap_frame));
    memset(tf, 0, sizeof(struct exception_trap_frame));

    tf->elr_el1 = entry_point;
    tf->spsr_el1 = SPSR_EL0_USER;
    tf->sp_el0 = (vaddr_stack + (stack_pages * PAGE_SIZE)) & ~15UL;

    process_table[pid].context.sp = (unsigned long)tf;
    process_table[pid].context.lr = (unsigned long)ret_to_user;

    for (size_t i = 0; i < VFS_MAX_FDS; i++)
    {
        process_table[pid].fd_table[i] = NULL;
    }
    vfs_open_pid("/dev/uart", VFS_O_RDONLY, pid);
    vfs_open_pid("/dev/uart", VFS_O_WRONLY, pid);
    vfs_open_pid("/dev/uart", VFS_O_WRONLY, pid);

    struct task* t = sched_create_user_task(process_table[pid].context.sp, process_table[pid].context.lr, pid);
    process_table[pid].main_task = t;
    enqueue_ready(get_core_id(), t);

    printf("[PROCESS] Loaded ELF %s for PID %d, entry at 0x%lx\n", path, pid, entry_point);
    return PERS_SUCCESS;
}

/*
 * Replaces the current process's image with a new executable (exec syscall).
 */
int process_exec(const char* path)
{
    int pid = process_find_current();
    if (pid < 0)
    {
        return pid;
    }

    struct process* p = &process_table[pid];

    unsigned long* new_pgd = mmu_create_user_pgd();
    if (!new_pgd)
    {
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    uint64_t entry_point;
    if (elf_load(path, new_pgd, &entry_point) != 0)
    {
        mmu_destroy_user_pgd(new_pgd);
        return -PERS_ERR_EXECUTABLE_FORMAT_ERROR;
    }

    struct va_allocator new_va;
    va_init(&new_va);
    size_t stack_pages = 32;

    uintptr_t vaddr_stack = process_va_alloc(&new_va, stack_pages);
    if (!vaddr_stack)
    {
        mmu_destroy_user_pgd(new_pgd);
        return -PERS_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < stack_pages; i++)
    {
        void* page = pmm_alloc_page();
        if (!page)
        {
            PANIC("Out of memory for exec stack");
        }
        mmu_user_map_page(new_pgd, vaddr_stack + i * PAGE_SIZE, V2P(page), MMU_PAGE_USER_DATA);
    }

    unsigned long* old_pgd = p->user_pgd;

    p->user_pgd = new_pgd;
    p->vaddr_code = entry_point;
    p->vaddr_user_stack = vaddr_stack;
    p->va = new_va;

    /* Reset signal handlers that are not ignored */
    for (int i = 0; i < SIGNAL_COUNT; i++)
    {
        if (p->signal_handlers[i] != SIGNAL_IGN)
        {
            p->signal_handlers[i] = SIGNAL_DFL;
        }
    }
    p->pending_signals = 0;
    p->sig_restorer = 0;

    mmu_switch_user(new_pgd, p->asid);

    unsigned long asid_field = ((unsigned long)(p->asid & 0xFFFFUL) << 48);
    asm volatile("dsb ish" ::: "memory");
    asm volatile("tlbi aside1is, %0" ::"r"(asid_field));
    asm volatile("dsb ish" ::: "memory");
    asm volatile("isb");

    if (old_pgd)
    {
        mmu_destroy_user_pgd(old_pgd);
    }

    uintptr_t kernel_stack_top = p->vaddr_kernel_stack + SCHED_TASK_STACK_SIZE;
    struct exception_trap_frame* tf =
        (struct exception_trap_frame*)(kernel_stack_top - sizeof(struct exception_trap_frame));

    memset(tf, 0, sizeof(struct exception_trap_frame));
    tf->elr_el1 = entry_point;
    tf->spsr_el1 = SPSR_EL0_USER;
    tf->sp_el0 = (vaddr_stack + (stack_pages * PAGE_SIZE)) & ~15UL;

    p->context.sp = (unsigned long)tf;
    p->context.lr = (unsigned long)ret_to_user;

    struct task* curr_task = sched_get_current();
    if (curr_task)
    {
        curr_task->context.sp = (unsigned long)tf;
        curr_task->context.lr = (unsigned long)ret_to_user;
        curr_task->ttbr0 = V2P(new_pgd) | ((p->asid & 0xFFFFUL) << 48);
        curr_task->skip_signals =
            1;  // ← add this to defer signal handling until we're back in user mode with the new image
    }

    return PERS_SUCCESS;
}

/*
 * Terminates a process and reclaims its resources.
 */
void process_exit(uint32_t pid, int exit_status)
{
    if (pid >= PROCESS_TABLE_SIZE)
        return;

    /* Use atomic exchange to ensure only one core/task proceeds with cleanup */
    process_state_t expected = PROCESS_STATE_RUNNING;
    if (!__atomic_compare_exchange_n(&process_table[pid].state,
                                     &expected,
                                     PROCESS_STATE_DEAD, /* Temporary state during cleanup */
                                     0,
                                     __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST))
    {
        return;
    }

    unsigned long flags = spin_lock_irqsave(&process_table_lock);

    printf("[PROCESS] PID %d exiting with status %d. Reclaiming resources...\n", pid, exit_status);

    // Reparent orphans immediately to evade dead-struct-task Use-After-Free
    for (int i = 0; i < PROCESS_TABLE_SIZE; i++)
    {
        if (i != (int)pid && process_table[i].state != PROCESS_STATE_EMPTY && process_table[i].parent_pid == pid)
        {
            process_table[i].parent_pid = 1;
        }
    }

    // Realigned FD checks under specific atomic lock scoping
    spin_lock(&process_table[pid].fd_lock);
    for (int i = 0; i < VFS_MAX_FDS; i++)
    {
        struct vfs_file* f = process_table[pid].fd_table[i];
        if (f)
        {
            process_table[pid].fd_table[i] = NULL;
            if (atomic_dec_and_test(&f->refcount))
            {
                vfs_vnode_put(f->node);
                slab_free(f);
            }
        }
    }
    spin_unlock(&process_table[pid].fd_lock);

    unsigned long* pgd = process_table[pid].user_pgd;
    process_table[pid].user_pgd = NULL;

    if (process_table[pid].cwd)
    {
        vfs_vnode_put(process_table[pid].cwd);
        process_table[pid].cwd = NULL;
    }

    process_table[pid].va.count = 0;
    process_table[pid].exit_status = exit_status;
    process_table[pid].state = PROCESS_STATE_ZOMBIE;

    // void* kstack = (void*)process_table[pid].vaddr_kernel_stack;
    process_table[pid].vaddr_kernel_stack = 0;

    uint32_t ppid = process_table[pid].parent_pid;
    if (ppid != 0 && process_table[ppid].state != PROCESS_STATE_EMPTY)
    {
        sched_unblock(process_table[ppid].main_task);
    }
    spin_unlock_irqrestore(&process_table_lock, flags);

    if (pgd)
    {
        mmu_destroy_user_pgd(pgd);
    }
}

/*
 * Duplicates the current process (fork syscall).
 */
int process_fork(struct exception_trap_frame* parent_tf)
{
    int parent_pid = process_find_current();
    if (parent_pid < 0)
    {
        return parent_pid;
    }
    struct process* parent = &process_table[parent_pid];

    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    int child_pid = -1;
    for (int i = 1; i < PROCESS_TABLE_SIZE; i++)
    {
        if (process_table[i].state == PROCESS_STATE_EMPTY)
        {
            child_pid = i;
            process_table[i].state = PROCESS_STATE_RUNNING;  // Reserve it
            break;
        }
    }
    spin_unlock_irqrestore(&process_table_lock, flags);

    if (child_pid == -1)
    {
        return -PERS_ERR_OUT_OF_RESOURCES;
    }

    struct process* child = &process_table[child_pid];
    uint32_t saved_pid = (uint32_t)child_pid;
    process_state_t saved_state = child->state;
    memset(child, 0, sizeof(struct process));
    child->pid = saved_pid;
    child->state = saved_state;

    child->fd_lock = (spinlock_t)SPINLOCK_INIT;

    unsigned long* child_pgd = mmu_copy_user_pgd(parent->user_pgd);
    if (!child_pgd)
    {
        spin_lock_irqsave(&process_table_lock);
        child->state = PROCESS_STATE_EMPTY;
        spin_unlock_irqrestore(&process_table_lock, flags);
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    child->pid = (uint32_t)child_pid;
    child->user_pgd = child_pgd;
    child->asid = (uint32_t)child_pid;
    child->ttbr0 = V2P(child_pgd) | ((uint64_t)child->asid << 48);
    child->vaddr_code = parent->vaddr_code;
    child->vaddr_user_stack = parent->vaddr_user_stack;
    child->va = parent->va;
    child->parent_pid = (uint32_t)parent_pid;
    child->main_task = NULL;

    /* Copy signal state */
    memcpy(child->signal_handlers, parent->signal_handlers, sizeof(child->signal_handlers));
    child->sig_restorer = parent->sig_restorer;
    child->pending_signals = 0;

    if (parent->cwd)

    {
        child->cwd = parent->cwd;
        atomic_inc(&child->cwd->refcount);
    }

    spin_lock(&parent->fd_lock);
    for (int i = 0; i < VFS_MAX_FDS; i++)
    {
        if (parent->fd_table[i])
        {
            child->fd_table[i] = parent->fd_table[i];
            atomic_inc(&child->fd_table[i]->refcount);
        }
        else
        {
            child->fd_table[i] = NULL;
        }
    }
    spin_unlock(&parent->fd_lock);

    void* kstack = alloc_kernel_stack();
    if (!kstack)
    {
        mmu_destroy_user_pgd(child_pgd);
        child->state = PROCESS_STATE_EMPTY;
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    child->vaddr_kernel_stack = (uintptr_t)kstack;
    child->paddr_kernel_stack = V2P(kstack);

    uintptr_t kernel_stack_top = (uintptr_t)kstack + SCHED_TASK_STACK_SIZE;
    struct exception_trap_frame* child_tf =
        (struct exception_trap_frame*)(kernel_stack_top - sizeof(struct exception_trap_frame));
    memcpy(child_tf, parent_tf, sizeof(struct exception_trap_frame));

    child_tf->x[0] = 0;

    child->context.sp = (unsigned long)child_tf;
    child->context.lr = (unsigned long)ret_to_user;

    child->state = PROCESS_STATE_RUNNING;
    struct task* t = sched_create_user_task(child->context.sp, child->context.lr, (uint32_t)child_pid);
    if (!t)
    {
        pmm_free_pages((void*)((uintptr_t)kstack - PAGE_SIZE), SCHED_STACK_PAGES);
        mmu_destroy_user_pgd(child_pgd);

        spin_lock(&child->fd_lock);
        for (int i = 0; i < VFS_MAX_FDS; i++)
        {
            if (child->fd_table[i])
            {
                if (atomic_dec_and_test(&child->fd_table[i]->refcount))
                {
                    vfs_vnode_put(child->fd_table[i]->node);
                    slab_free(child->fd_table[i]);
                }
                child->fd_table[i] = NULL;
            }
        }
        spin_unlock(&child->fd_lock);

        if (child->cwd)
        {
            vfs_vnode_put(child->cwd);
            child->cwd = NULL;
        }

        unsigned long f2 = spin_lock_irqsave(&process_table_lock);
        child->state = PROCESS_STATE_EMPTY;
        spin_unlock_irqrestore(&process_table_lock, f2);
        return -PERS_ERR_OUT_OF_MEMORY;
    }
    child->main_task = t;
    enqueue_ready(get_core_id(), t);

    printf("[PROCESS] PID %d forked child PID %d\n", parent_pid, child_pid);
    return child_pid;
}

/*
 * Waits for a specific child process (or any) to terminate.
 */
int process_waitpid(int pid, int* status)
{
    int parent_pid = process_find_current();
    if (parent_pid < 0)
    {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }

    while (1)
    {
        unsigned long irqf = irq_save();
        spin_lock(&process_table_lock);

        struct task* curr = sched_get_current();
        if (curr)
        {
            curr->state = SCHED_TASK_BLOCKED;
        }

        int has_children = 0;
        for (int i = 0; i < PROCESS_TABLE_SIZE; i++)
        {
            if (process_table[i].state == PROCESS_STATE_EMPTY)
            {
                continue;
            }
            if (process_table[i].parent_pid != (uint32_t)parent_pid)
            {
                continue;
            }
            if (pid != -1 && (int)process_table[i].pid != pid)
            {
                continue;
            }

            has_children = 1;

            if (process_table[i].state == PROCESS_STATE_ZOMBIE)
            {
                if (status)
                {
                    *status = process_table[i].exit_status;
                }
                int found_pid = (int)process_table[i].pid;
                process_table[i].state = PROCESS_STATE_EMPTY;

                if (curr)
                {
                    curr->state = SCHED_TASK_RUNNING;
                }

                spin_unlock(&process_table_lock);
                irq_restore(irqf);
                return found_pid;
            }
        }

        if (!has_children)
        {
            if (curr)
            {
                curr->state = SCHED_TASK_RUNNING;
            }
            spin_unlock(&process_table_lock);
            irq_restore(irqf);
            return -PERS_ERR_NO_SUCH_PROCESS;
        }

        spin_unlock(&process_table_lock);

        schedule();
        irq_restore(irqf);
    }
}

/*
 * Retrieves the hardware TTBR0 value for a given PID.
 */
unsigned long process_get_ttbr0(uint32_t pid)
{
    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    if (pid >= PROCESS_TABLE_SIZE)
    {
        spin_unlock_irqrestore(&process_table_lock, flags);
        return mmu_kernel_ttbr0();
    }
    if (process_table[pid].state == PROCESS_STATE_EMPTY || process_table[pid].state == PROCESS_STATE_DEAD)
    {
        spin_unlock_irqrestore(&process_table_lock, flags);
        return mmu_kernel_ttbr0();
    }

    unsigned long ttbr0 = process_table[pid].ttbr0;
    spin_unlock_irqrestore(&process_table_lock, flags);
    return ttbr0;
}

/*
 * Jumps to user-space at the specified code and stack addresses.
 */
void process_drop_to_user(void* code_vaddr, void* stack_vaddr)
{
    uint64_t spsr = 0;

    asm volatile("msr spsr_el1, %0\n"
                 "msr elr_el1, %1\n"
                 "msr sp_el0, %2\n"
                 "eret\n"
                 :
                 : "r"(spsr), "r"(code_vaddr), "r"(stack_vaddr)
                 : "memory");
}
