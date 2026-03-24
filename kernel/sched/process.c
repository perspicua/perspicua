/*
 * kernel/sched/process.c
 *
 * Core process management, lifecycle, and task definitions.
 * Handles user-space execution, process tracking, and address spaces.
 *
 * Lock ordering (always acquire in this order to prevent deadlock):
 *   1. process_table_lock  (global, coarse)
 *   2. process.fd_lock     (per-process, fine)
 *
 * IRQ discipline:
 *   - process_table_lock is always taken with irqsave/irqrestore because
 *     it may be acquired from an interrupt context (e.g. a timer-driven
 *     scheduler tick calling process_exit via a signal path).
 *   - fd_lock is a pure spin_lock; it is never acquired from IRQ context.
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

/* --------------------------------------------------------------------------
 * Globals
 * -------------------------------------------------------------------------- */

spinlock_t process_table_lock = SPINLOCK_INIT;
struct process process_table[PROCESS_TABLE_SIZE];

/* Defined in arch/entry.S — performs ERET to EL0 using the trap frame that
 * sits at the top of the kernel stack. */
extern void ret_to_user(void);

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/*
 * va_init — reset a virtual-address allocator to its initial state.
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
 * alloc_kernel_stack — allocate SCHED_STACK_PAGES physical pages for a
 * kernel stack.  The first page is unmapped to act as a guard page; the
 * second page starts with the stack canary.
 *
 * Returns a pointer to the first *usable* byte (page 1), or NULL on OOM.
 *
 * Callers must free with:
 *   pmm_free_pages((void *)((uintptr_t)returned_ptr - PAGE_SIZE),
 *                  SCHED_STACK_PAGES);
 */
static void* alloc_kernel_stack(void)
{
    unsigned char* base = (unsigned char*)pmm_alloc_pages(SCHED_STACK_PAGES);
    if (!base)
        return NULL;

    /* Guard page — any underflow will immediately fault. */
    mmu_unmap_page((unsigned long)base);

    /* Write the canary at the very bottom of the usable region. */
    *(unsigned long*)(base + PAGE_SIZE) = SCHED_STACK_CANARY;

    return base + PAGE_SIZE;
}

/*
 * free_kernel_stack — release a kernel stack obtained via alloc_kernel_stack.
 * Pass the value returned by alloc_kernel_stack (i.e. base + PAGE_SIZE).
 */
static void free_kernel_stack(void* stack_base)
{
    if (!stack_base)
        return;
    /* Step back over the guard page before handing to the PMM. */
    void* alloc_base = (void*)((uintptr_t)stack_base - PAGE_SIZE);

    /*
     * RE-MAP the guard page!
     * pmm_free_pages (and future pmm_alloc_pages) will attempt to zero this
     * memory. If it remains unmapped, the kernel will panic.
     */
}

/*
 * open_std_fds — open stdin/stdout/stderr on /dev/uart for a process.
 * This is identical for every new process so it lives in one place.
 */
static void open_std_fds(uint32_t pid)
{
    vfs_open_pid("/dev/uart", VFS_O_RDONLY, pid); /* fd 0 — stdin  */
    vfs_open_pid("/dev/uart", VFS_O_WRONLY, pid); /* fd 1 — stdout */
    vfs_open_pid("/dev/uart", VFS_O_WRONLY, pid); /* fd 2 — stderr */
}

/*
 * close_all_fds — close every open file descriptor belonging to a process.
 * Caller must NOT hold p->fd_lock on entry; this function acquires and
 * releases it internally.
 */
static void close_all_fds(struct process* p)
{
    spin_lock(&p->fd_lock);
    for (int i = 0; i < VFS_MAX_FDS; i++)
    {
        struct vfs_file* f = p->fd_table[i];
        if (!f)
            continue;

        p->fd_table[i] = NULL;
        if (atomic_dec_and_test(&f->refcount))
        {
            if (f->node && f->node->ops && f->node->ops->close)
                f->node->ops->close(f);
            if (f->node)
                vfs_vnode_put(f->node);
            slab_free(f);
        }
    }
    spin_unlock(&p->fd_lock);
}

/*
 * build_trap_frame — fill in the exception trap frame at the top of the
 * kernel stack so that ret_to_user lands at entry_pc with sp_el0 set to
 * user_sp_top (already 16-byte aligned by caller).
 *
 * Returns a pointer to the trap frame; the frame resides inside the kernel
 * stack allocation and must not be freed separately.
 */
static struct exception_trap_frame* build_trap_frame(uintptr_t kstack_base, uint64_t entry_pc, uintptr_t user_sp_top)
{
    uintptr_t kernel_stack_top = kstack_base + SCHED_TASK_STACK_SIZE;
    uintptr_t tf_addr = (kernel_stack_top - sizeof(struct exception_trap_frame)) & ~15UL;
    struct exception_trap_frame* tf = (struct exception_trap_frame*)tf_addr;
    memset(tf, 0, sizeof(*tf));
    tf->elr_el1 = entry_pc;
    tf->spsr_el1 = SPSR_EL0_USER;
    tf->sp_el0 = user_sp_top & ~15UL;
    return tf;
}

/*
 * setup_user_stack — allocate 'pages' physical pages, map them into 'pgd'
 * starting at the virtual address returned by process_va_alloc, and return
 * the virtual base address.  Returns 0 on failure.
 */
static uintptr_t setup_user_stack(struct va_allocator* va, unsigned long* pgd, size_t pages)
{
    uintptr_t vbase = process_va_alloc(va, pages);
    if (!vbase)
        return 0;

    for (size_t i = 0; i < pages; i++)
    {
        void* page = pmm_alloc_page();
        if (!page)
            PANIC("process: OOM allocating user stack page");
        mmu_user_map_page(pgd, vbase + i * PAGE_SIZE, V2P(page), MMU_PAGE_USER_DATA);
    }
    return vbase;
}

/* --------------------------------------------------------------------------
 * Public API — memory management
 * -------------------------------------------------------------------------- */

/*
 * process_flush_icache_range — clean D-cache then invalidate I-cache for
 * the given virtual range (AArch64, cache-line size assumed 64 bytes).
 */
void process_flush_icache_range(void* start, size_t size)
{
    unsigned long addr = (unsigned long)start & ~63UL;
    unsigned long end = ((unsigned long)start + size + 63UL) & ~63UL;

    for (unsigned long a = addr; a < end; a += 64)
        asm volatile("dc cvau, %0" ::"r"(a) : "memory");
    asm volatile("dsb ish" ::: "memory");

    for (unsigned long a = addr; a < end; a += 64)
        asm volatile("ic ivau, %0" ::"r"(a) : "memory");
    asm volatile("dsb ish" ::: "memory");
    asm volatile("isb");
}

/*
 * process_va_alloc — reserve the next 'pages' pages of user virtual address
 * space in the allocator and return the base address.  Returns 0 on failure.
 */
uintptr_t process_va_alloc(struct va_allocator* va, size_t pages)
{
    if (pages == 0 || va->count >= USER_VA_MAX_REGIONS)
        return 0;

    uintptr_t base = va->next_va;
    size_t size = pages * PAGE_SIZE;

    /* Overflow and 39-bit VA ceiling check. */
    if (base + size < base)
        return 0;
    if (base + size > 0x8000000000ULL)
        return 0;

    va->regions[va->count].base = base;
    va->regions[va->count].pages = pages;
    va->count++;
    va->next_va = base + size;
    return base;
}

/*
 * process_va_free — remove a previously allocated region from the allocator.
 * Does NOT unmap pages; callers must do so before or after.
 */
void process_va_free(struct va_allocator* va, uintptr_t base)
{
    for (size_t i = 0; i < va->count; i++)
    {
        if (va->regions[i].base != base)
            continue;
        /* Compact the array. */
        for (size_t j = i; j + 1 < va->count; j++)
            va->regions[j] = va->regions[j + 1];
        va->count--;
        return;
    }
}

/* --------------------------------------------------------------------------
 * Public API — identity
 * -------------------------------------------------------------------------- */

/*
 * process_find_current — return the PID of the task currently running on
 * this core, or a negative error code if there is no current task.
 */
int process_find_current(void)
{
    struct task* t = sched_get_current();
    if (!t)
        return -PERS_ERR_NO_SUCH_PROCESS;
    return (int)t->pid;
}

/*
 * process_get_ttbr0 — return the hardware-ready TTBR0 value for a PID.
 * Falls back to the kernel TTBR0 if the PID is invalid or the process is
 * not alive.
 */
unsigned long process_get_ttbr0(uint32_t pid)
{
    unsigned long flags = spin_lock_irqsave(&process_table_lock);

    if (pid >= PROCESS_TABLE_SIZE || process_table[pid].state == PROCESS_STATE_EMPTY
        || process_table[pid].state == PROCESS_STATE_DEAD)
    {
        spin_unlock_irqrestore(&process_table_lock, flags);
        return mmu_kernel_ttbr0();
    }

    unsigned long ttbr0 = process_table[pid].ttbr0;
    spin_unlock_irqrestore(&process_table_lock, flags);
    return ttbr0;
}

/* --------------------------------------------------------------------------
 * Public API — initialisation
 * -------------------------------------------------------------------------- */

/*
 * process_init — zero the process table and mark every slot EMPTY.
 * Slot 0 is reserved for the kernel boot task.
 */
void process_init(void)
{
    memset(process_table, 0, sizeof(process_table));

    for (size_t i = 0; i < PROCESS_TABLE_SIZE; i++)
    {
        process_table[i].state = PROCESS_STATE_EMPTY;
        process_table[i].fd_lock = (spinlock_t)SPINLOCK_INIT;
    }

    /* PID 0 — the kernel itself. */
    process_table[0].pid = 0;
    process_table[0].state = PROCESS_STATE_RUNNING;
    process_table[0].user_pgd = NULL;
    process_table[0].asid = 0;

    printf("[ PROC ] Process management initialized (%zu slots)\n", (size_t)PROCESS_TABLE_SIZE);
}

/* --------------------------------------------------------------------------
 * Public API — lifecycle
 * -------------------------------------------------------------------------- */

/*
 * process_create — create a user process from a raw in-memory code blob.
 * Intended for the very first user task launched during boot before the
 * filesystem is available.
 *
 * The blob must fit in a single page (≤ PAGE_SIZE bytes).
 */
void process_create(void* code_ptr, size_t code_size, uint32_t pid)
{
    /* ---- Validate arguments ---- */
    if (pid >= PROCESS_TABLE_SIZE)
    {
        printf("[PROCESS] process_create: PID %u out of range\n", pid);
        return;
    }
    if (code_size == 0 || code_size > PAGE_SIZE)
    {
        printf("[PROCESS] process_create: invalid code_size %zu\n", code_size);
        return;
    }

    /* ---- Reserve the table slot ---- */
    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    if (process_table[pid].state != PROCESS_STATE_EMPTY)
    {
        spin_unlock_irqrestore(&process_table_lock, flags);
        printf("[PROCESS] process_create: PID %u already in use\n", pid);
        return;
    }
    process_table[pid].state = PROCESS_STATE_RUNNING; /* reserved */
    spin_unlock_irqrestore(&process_table_lock, flags);

    struct process* p = &process_table[pid];

    /* ---- Virtual address space ---- */
    va_init(&p->va);

    /* ---- Page table ---- */
    unsigned long* user_pgd = mmu_create_user_pgd();
    if (!user_pgd)
        PANIC("process_create: failed to create user PGD");

    /* ---- Code page ---- */
    void* code_page = pmm_alloc_page();
    if (!code_page)
        PANIC("process_create: OOM for code page");

    uintptr_t vaddr_code = process_va_alloc(&p->va, 1);
    if (!vaddr_code)
        PANIC("process_create: VA space exhausted for code");

    mmu_user_map_page(user_pgd, vaddr_code, V2P(code_page), MMU_PAGE_USER_CODE);
    memcpy(code_page, code_ptr, code_size);
    process_flush_icache_range(code_page, code_size);
    asm volatile("ic ialluis\n dsb ish\n isb");

    /* ---- User stack ---- */
    uintptr_t vaddr_user_stack = setup_user_stack(&p->va, user_pgd, PROCESS_USER_STACK_PAGES);
    if (!vaddr_user_stack)
        PANIC("process_create: VA space exhausted for user stack");

    /* ---- Kernel stack ---- */
    void* kstack = alloc_kernel_stack();
    if (!kstack)
        PANIC("process_create: OOM for kernel stack");

    /* ---- Fill in the PCB ---- */
    p->pid = pid;
    p->user_pgd = user_pgd;
    p->asid = pid;
    p->ttbr0 = V2P(user_pgd) | ((uint64_t)pid << 48);
    p->vaddr_code = vaddr_code;
    p->paddr_code = V2P(code_page);
    p->vaddr_user_stack = vaddr_user_stack;
    p->vaddr_kernel_stack = (uintptr_t)kstack;
    p->paddr_kernel_stack = V2P(kstack);

    /* ---- Trap frame ---- */
    uintptr_t user_sp_top = vaddr_user_stack + PROCESS_USER_STACK_PAGES * PAGE_SIZE;
    struct exception_trap_frame* tf = build_trap_frame((uintptr_t)kstack, (uint64_t)vaddr_code, user_sp_top);

    p->context.sp = (unsigned long)tf;
    p->context.lr = (unsigned long)ret_to_user;

    /* ---- Filesystem context ---- */
    int err;
    p->cwd = vfs_resolve_path("/", NULL, &err);
    for (int i = 0; i < VFS_MAX_FDS; i++)
        p->fd_table[i] = NULL;
    open_std_fds(pid);

    /* ---- Signal state ---- */
    memset(p->signal_handlers, 0, sizeof(p->signal_handlers));
    for (int i = 0; i < SIGNAL_COUNT; i++)
        p->signal_handlers[i].sa_handler = SIGNAL_DFL;
    p->pending_signals = 0;
    p->blocked_signals = 0;
    p->default_sigrestorer = 0;

    /* ---- Schedule ---- */
    struct task* t = sched_create_user_task(p->context.sp, p->context.lr, p->vaddr_kernel_stack, pid);
    if (!t)
        PANIC("process_create: sched_create_user_task failed");
    p->main_task = t;
    enqueue_ready(get_core_id(), t);
}

/*
 * process_create_from_file — load an ELF executable from disk and create a
 * user process for it.
 *
 * Returns PERS_SUCCESS or a negative PERS_ERR_* code.
 */
int process_create_from_file(const char* path, uint32_t pid)
{
    /* ---- Validate & reserve ---- */
    if (pid >= PROCESS_TABLE_SIZE)
        return -PERS_ERR_INVALID_ARGUMENT;

    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    if (process_table[pid].state != PROCESS_STATE_EMPTY)
    {
        spin_unlock_irqrestore(&process_table_lock, flags);
        return -PERS_ERR_ALREADY_EXISTS;
    }
    process_table[pid].state = PROCESS_STATE_RUNNING; /* reserved */
    spin_unlock_irqrestore(&process_table_lock, flags);

    struct process* p = &process_table[pid];
    va_init(&p->va);

    /* ---- Page table ---- */
    unsigned long* user_pgd = mmu_create_user_pgd();
    if (!user_pgd)
    {
        p->state = PROCESS_STATE_EMPTY;
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    /* ---- ELF load ---- */
    uint64_t entry_point;
    if (elf_load(path, user_pgd, &entry_point) != 0)
    {
        mmu_destroy_user_pgd(user_pgd);
        p->state = PROCESS_STATE_EMPTY;
        return -PERS_ERR_EXECUTABLE_FORMAT_ERROR;
    }

    /* ---- User stack ---- */
    uintptr_t vaddr_stack = setup_user_stack(&p->va, user_pgd, 32);
    if (!vaddr_stack)
    {
        mmu_destroy_user_pgd(user_pgd);
        p->state = PROCESS_STATE_EMPTY;
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    /* ---- Kernel stack ---- */
    void* kstack = alloc_kernel_stack();
    if (!kstack)
    {
        mmu_destroy_user_pgd(user_pgd);
        p->state = PROCESS_STATE_EMPTY;
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    /* ---- Fill in the PCB ---- */
    p->pid = pid;
    p->user_pgd = user_pgd;
    p->asid = pid;
    p->ttbr0 = V2P(user_pgd) | ((uint64_t)pid << 48);
    p->vaddr_code = (uintptr_t)entry_point;
    p->vaddr_user_stack = vaddr_stack;
    p->vaddr_kernel_stack = (uintptr_t)kstack;
    p->paddr_kernel_stack = V2P(kstack);

    /* ---- Trap frame ---- */
    uintptr_t user_sp_top = vaddr_stack + 32 * PAGE_SIZE;
    struct exception_trap_frame* tf = build_trap_frame((uintptr_t)kstack, entry_point, user_sp_top);

    p->context.sp = (unsigned long)tf;
    p->context.lr = (unsigned long)ret_to_user;

    /* ---- Filesystem context ---- */
    int err;
    p->cwd = vfs_resolve_path("/", NULL, &err);
    for (int i = 0; i < VFS_MAX_FDS; i++)
        p->fd_table[i] = NULL;
    open_std_fds(pid);

    /* ---- Signal state ---- */
    memset(p->signal_handlers, 0, sizeof(p->signal_handlers));
    for (int i = 0; i < SIGNAL_COUNT; i++)
        p->signal_handlers[i].sa_handler = SIGNAL_DFL;
    p->pending_signals = 0;
    p->blocked_signals = 0;
    p->default_sigrestorer = 0;

    /* ---- Schedule ---- */
    struct task* t = sched_create_user_task(p->context.sp, p->context.lr, p->vaddr_kernel_stack, pid);
    if (!t)
    {
        close_all_fds(p);
        if (p->cwd)
        {
            vfs_vnode_put(p->cwd);
            p->cwd = NULL;
        }
        free_kernel_stack(kstack);
        mmu_destroy_user_pgd(user_pgd);
        p->state = PROCESS_STATE_EMPTY;
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    p->main_task = t;
    enqueue_ready(get_core_id(), t);

    printf("[PROCESS] Loaded ELF '%s' for PID %u, entry 0x%llx\n", path, pid, (unsigned long long)entry_point);
    return PERS_SUCCESS;
}

/*
 * process_exec — replace the current process image with a new ELF executable.
 *
 * The function:
 *   1. Loads the new ELF into a fresh page table.
 *   2. Allocates a new user stack.
 *   3. Resets signal handlers (SIG_IGN preserved, everything else -> SIG_DFL).
 *   4. Switches TTBR0, invalidates ASID-tagged TLB entries.
 *   5. Destroys the old page table.
 *   6. Rewrites the trap frame so the next return-to-user lands in the new
 *      image.  The kernel stack itself is reused.
 *
 * Returns PERS_SUCCESS or a negative error code.
 */
int process_exec(const char* path)
{
    int pid = process_find_current();
    if (pid < 0)
        return pid;

    struct process* p = &process_table[pid];

    /* ---- Load new ELF into fresh page table ---- */
    unsigned long* new_pgd = mmu_create_user_pgd();
    if (!new_pgd)
        return -PERS_ERR_OUT_OF_MEMORY;

    uint64_t entry_point;
    if (elf_load(path, new_pgd, &entry_point) != 0)
    {
        mmu_destroy_user_pgd(new_pgd);
        return -PERS_ERR_EXECUTABLE_FORMAT_ERROR;
    }

    /* ---- New user stack ---- */
    struct va_allocator new_va;
    va_init(&new_va);

    uintptr_t new_stack = setup_user_stack(&new_va, new_pgd, 32);
    if (!new_stack)
    {
        mmu_destroy_user_pgd(new_pgd);
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    /* ---- Close FDs with O_CLOEXEC set ---- */
    /* NOTE: a POSIX exec closes all O_CLOEXEC fds.  For now we close all
     * fds for simplicity; extend this once VFS tracks the CLOEXEC flag. */
    close_all_fds(p);
    open_std_fds((uint32_t)pid);

    /* ---- Reset signal handlers (SIG_IGN is preserved) ---- */
    for (int i = 0; i < SIGNAL_COUNT; i++)
    {
        if (p->signal_handlers[i].sa_handler != SIGNAL_IGN)
        {
            memset(&p->signal_handlers[i], 0, sizeof(struct sigaction));
            p->signal_handlers[i].sa_handler = SIGNAL_DFL;
        }
    }
    p->pending_signals = 0;
    p->default_sigrestorer = 0;

    /* ---- Swap address space ---- */
    unsigned long* old_pgd = p->user_pgd;
    p->user_pgd = new_pgd;
    p->vaddr_code = (uintptr_t)entry_point;
    p->vaddr_user_stack = new_stack;
    p->va = new_va;
    p->ttbr0 = V2P(new_pgd) | ((uint64_t)(p->asid & 0xFFFFUL) << 48);

    /* Switch hardware TTBR0 and flush stale ASID-tagged TLB entries. */
    mmu_switch_user(new_pgd, p->asid);
    unsigned long asid_field = (unsigned long)(p->asid & 0xFFFFUL) << 48;
    asm volatile("dsb ish" ::: "memory");
    asm volatile("tlbi aside1is, %0" ::"r"(asid_field));
    asm volatile("dsb ish" ::: "memory");
    asm volatile("isb");

    /* Destroy old address space now that TTBR0 no longer points to it. */
    if (old_pgd)
        mmu_destroy_user_pgd(old_pgd);

    /* ---- Rebuild trap frame on the existing kernel stack ---- */
    uintptr_t user_sp_top = new_stack + 32 * PAGE_SIZE;
    struct exception_trap_frame* tf = build_trap_frame(p->vaddr_kernel_stack, entry_point, user_sp_top);

    p->context.sp = (unsigned long)tf;
    p->context.lr = (unsigned long)ret_to_user;

    /* Propagate the new context into the scheduler task struct so that the
     * context-switch path uses the correct stack pointer and TTBR0.
     * skip_signals defers signal delivery until we have fully returned to
     * the new user image. */
    struct task* curr = sched_get_current();
    if (curr)
    {
        curr->context.sp = (unsigned long)tf;
        curr->context.lr = (unsigned long)ret_to_user;
        curr->ttbr0 = p->ttbr0;
        curr->skip_signals = 1;
    }

    printf("[PROCESS] exec '%s' for PID %d, entry 0x%llx\n", path, pid, (unsigned long long)entry_point);
    return PERS_SUCCESS;
}

/*
 * process_exit — terminate a process, reclaim its resources, and transition
 * it to ZOMBIE so a waiting parent can collect the exit status.
 *
 * Safe to call from any core; the compare-exchange at the top ensures only
 * one call proceeds through cleanup regardless of concurrent exits.
 */
void process_exit(uint32_t pid, int exit_status)
{
    if (pid >= PROCESS_TABLE_SIZE)
        return;

    process_state_t expected = PROCESS_STATE_RUNNING;
    if (!__atomic_compare_exchange_n(
            &process_table[pid].state, &expected, PROCESS_STATE_DEAD, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return;

    printf("[PROCESS] PID %u exiting with status %d\n", pid, exit_status);

    struct process* p = &process_table[pid];

    /* Reparent orphaned children to init (PID 1) */
    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    for (int i = 1; i < PROCESS_TABLE_SIZE; i++)
    {
        if (i == (int)pid)
            continue;
        if (process_table[i].state == PROCESS_STATE_EMPTY)
            continue;
        if (process_table[i].parent_pid == pid)
            process_table[i].parent_pid = 1;
    }
    spin_unlock_irqrestore(&process_table_lock, flags);

    close_all_fds(p);

    if (p->cwd)
    {
        vfs_vnode_put(p->cwd);
        p->cwd = NULL;
    }

    unsigned long* pgd = p->user_pgd;
    p->user_pgd = NULL;
    if (pgd)
    {
        unsigned long asid_field = (unsigned long)(p->asid & 0xFFFFUL) << 48;
        asm volatile("dsb ish");
        asm volatile("tlbi aside1is, %0" ::"r"(asid_field));
        asm volatile("dsb ish");
        asm volatile("isb");
        mmu_destroy_user_pgd(pgd);
    }

    /*
     * Do NOT touch vaddr_kernel_stack or paddr_kernel_stack here.
     * The kernel stack is freed by cleanup_dead_task() in schedule()
     * via the task struct's t->stack pointer, after the task has been
     * fully switched away from.
     */

    p->va.count = 0;
    p->exit_status = exit_status;

    flags = spin_lock_irqsave(&process_table_lock);
    p->state = PROCESS_STATE_ZOMBIE;

    uint32_t ppid = p->parent_pid;
    if (ppid != 0 && ppid < PROCESS_TABLE_SIZE && process_table[ppid].state != PROCESS_STATE_EMPTY
        && process_table[ppid].main_task != NULL)
    {
        sched_unblock(process_table[ppid].main_task);
    }
    spin_unlock_irqrestore(&process_table_lock, flags);

    struct task* dying = sched_get_current();
    if (dying)
        dying->state = SCHED_TASK_DEAD;

    schedule();
    __builtin_unreachable();
}

/*
 * process_fork — duplicate the calling process.
 *
 * The child gets:
 *   - A copy-on-write clone of the parent's user address space.
 *   - A fresh kernel stack with a copy of the parent's trap frame.
 *   - x0 = 0 in the trap frame (fork returns 0 in the child).
 *   - Copies of all open file descriptors (refcounts incremented).
 *   - A copy of the parent's signal handler table (pending signals cleared).
 *
 * Returns the child PID to the parent, 0 to the child, or a negative error.
 */
int process_fork(struct exception_trap_frame* parent_tf)
{
    int parent_pid = process_find_current();
    if (parent_pid < 0)
        return parent_pid;

    struct process* parent = &process_table[parent_pid];

    /* ---- Find a free slot ---- */
    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    int child_pid = -1;
    for (int i = 1; i < PROCESS_TABLE_SIZE; i++)
    {
        if (process_table[i].state == PROCESS_STATE_EMPTY)
        {
            child_pid = i;
            process_table[i].state = PROCESS_STATE_RUNNING; /* reserved */
            break;
        }
    }
    spin_unlock_irqrestore(&process_table_lock, flags);

    if (child_pid == -1)
        return -PERS_ERR_OUT_OF_RESOURCES;

    struct process* child = &process_table[child_pid];

    /* Zero the child PCB but restore what we just wrote. */
    memset(child, 0, sizeof(*child));
    child->pid = (uint32_t)child_pid;
    child->state = PROCESS_STATE_RUNNING;
    child->fd_lock = (spinlock_t)SPINLOCK_INIT;

    /* ---- Copy-on-write user address space ---- */
    unsigned long* child_pgd = mmu_copy_user_pgd(parent->user_pgd);
    if (!child_pgd)
    {
        child->state = PROCESS_STATE_EMPTY;
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    /* ---- Kernel stack for the child ---- */
    void* kstack = alloc_kernel_stack();
    if (!kstack)
    {
        mmu_destroy_user_pgd(child_pgd);
        child->state = PROCESS_STATE_EMPTY;
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    /* ---- Fill in child PCB ---- */
    child->user_pgd = child_pgd;
    child->asid = (uint32_t)child_pid;
    child->ttbr0 = V2P(child_pgd) | ((uint64_t)child_pid << 48);
    child->vaddr_code = parent->vaddr_code;
    child->vaddr_user_stack = parent->vaddr_user_stack;
    child->vaddr_kernel_stack = (uintptr_t)kstack;
    child->paddr_kernel_stack = V2P(kstack);
    child->va = parent->va;
    child->parent_pid = (uint32_t)parent_pid;

    /* ---- Signal state ---- */
    memcpy(child->signal_handlers, parent->signal_handlers, sizeof(child->signal_handlers));
    child->default_sigrestorer = parent->default_sigrestorer;
    child->pending_signals = 0; /* pending signals are not inherited */
    child->blocked_signals = parent->blocked_signals;

    /* ---- Filesystem context ---- */
    if (parent->cwd)
    {
        child->cwd = parent->cwd;
        atomic_inc(&child->cwd->refcount);
    }

    /* Duplicate file descriptor table under the parent fd_lock. */
    spin_lock(&parent->fd_lock);
    for (int i = 0; i < VFS_MAX_FDS; i++)
    {
        if (parent->fd_table[i])
        {
            child->fd_table[i] = parent->fd_table[i];
            atomic_inc(&child->fd_table[i]->refcount);
        }
    }
    spin_unlock(&parent->fd_lock);

    /* ---- Child trap frame (copy of parent, x0 = 0) ---- */
    uintptr_t kernel_stack_top = (uintptr_t)kstack + SCHED_TASK_STACK_SIZE;
    uintptr_t tf_addr = (kernel_stack_top - sizeof(struct exception_trap_frame)) & ~15UL;
    struct exception_trap_frame* child_tf = (struct exception_trap_frame*)tf_addr;
    memcpy(child_tf, parent_tf, sizeof(*child_tf));
    child_tf->x[0] = 0; /* fork() returns 0 in the child */

    child->context.sp = (unsigned long)child_tf;
    child->context.lr = (unsigned long)ret_to_user;

    /* ---- Create scheduler task ---- */
    struct task* t =
        sched_create_user_task(child->context.sp, child->context.lr, (uintptr_t)kstack, (uint32_t)child_pid);
    if (!t)
    {
        /* Clean up everything we allocated. */
        close_all_fds(child);
        if (child->cwd)
        {
            vfs_vnode_put(child->cwd);
            child->cwd = NULL;
        }
        free_kernel_stack(kstack);
        mmu_destroy_user_pgd(child_pgd);
        child->state = PROCESS_STATE_EMPTY;
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    child->main_task = t;
    enqueue_ready(get_core_id(), t);

    printf("[PROCESS] PID %d forked -> child PID %d\n", parent_pid, child_pid);
    return child_pid;
}

/*
 * process_waitpid — wait for a child to terminate and collect its exit status.
 *
 * pid == -1  -> wait for any child
 * pid  > 0  -> wait for that specific child
 *
 * Returns the PID of the reaped child on success, or a negative error code.
 *
 * The caller blocks via sched_block() whenever eligible children exist but
 * none has exited yet.  process_exit() calls sched_unblock() on the parent
 * task to wake it up.
 */
int process_waitpid(int pid, int* status)
{
    int parent_pid = process_find_current();
    if (parent_pid < 0)
        return -PERS_ERR_NO_SUCH_PROCESS;

    for (;;)
    {
        unsigned long irqf = irq_save();
        spin_lock(&process_table_lock);

        int has_children = 0;

        for (int i = 1; i < PROCESS_TABLE_SIZE; i++)
        {
            struct process* candidate = &process_table[i];

            if (candidate->state == PROCESS_STATE_EMPTY)
                continue;
            if (candidate->parent_pid != (uint32_t)parent_pid)
                continue;
            if (pid != -1 && (int)candidate->pid != pid)
                continue;

            has_children = 1;

            if (candidate->state == PROCESS_STATE_ZOMBIE)
            {
                int found_pid = (int)candidate->pid;
                if (status)
                    *status = candidate->exit_status;

                candidate->state = PROCESS_STATE_EMPTY;
                /* No stack free here — cleanup_dead_task() in schedule()
                 * already owns that via t->stack on the dying task struct. */

                spin_unlock(&process_table_lock);
                irq_restore(irqf);
                return found_pid;
            }
        }

        if (!has_children)
        {
            spin_unlock(&process_table_lock);
            irq_restore(irqf);
            return -PERS_ERR_NO_SUCH_PROCESS;
        }

        /*
         * Lost-wakeup fix: set BLOCKED *before* releasing the lock.
         * Any process_exit() firing after the unlock will call
         * sched_unblock(), see BLOCKED, and the CAS succeeds.
         * If we set BLOCKED after the unlock (old code), process_exit()
         * could fire between unlock and assignment, see RUNNING, and
         * silently drop the wakeup.
         */
        struct task* curr = sched_get_current();
        if (curr)
            curr->state = SCHED_TASK_BLOCKED;

        spin_unlock(&process_table_lock);

        /* Call schedule() directly, not sched_block(), because sched_block()
         * would overwrite the BLOCKED state we just set above. */
        schedule();
        irq_restore(irqf);
    }
} /* --------------------------------------------------------------------------
   * Public API — arch helpers
   * -------------------------------------------------------------------------- */

/*
 * process_drop_to_user — perform a bare ERET to EL0.
 *
 * Used only during early boot before the trap-frame mechanism is in place.
 * For normal process creation use process_create / process_create_from_file.
 */
void process_drop_to_user(void* code_vaddr, void* stack_vaddr)
{
    uint64_t spsr = SPSR_EL0_USER;
    asm volatile("msr spsr_el1, %0\n"
                 "msr elr_el1,  %1\n"
                 "msr sp_el0,   %2\n"
                 "eret\n"
                 :
                 : "r"(spsr), "r"(code_vaddr), "r"(stack_vaddr)
                 : "memory");
}
