/*
 * process.c - Core process management and lifecycle implementation.
 *
 * This module handles user-space execution, process tracking, and address
 * space management. It maintains the process table and enforces lock
 * ordering to prevent deadlocks.
 *
 * Lock Ordering: 1. process_table_lock (global) -> 2. process.fd_lock (per-process)
 */
#include "sched/process.h"

#include "mm/asid.h"
#include "uapi/wait.h"
#include "uapi/errors.h"
#include "arch/exception.h"
#include "mm/addr.h"
#include "core/elf.h"
#include "mm/mmu.h"
#include "mm/heap.h"
#include "panic.h"
#include "mm/pmm.h"
#include "sched/sched.h"
#include "mm/slab.h"
#include "stdio.h"
#include "string.h"
#include "core/timer.h"
#include "types.h"
#include "core/tty.h"
#include "fs/vfs.h"
#include "arch/uaccess.h"

spinlock_t process_table_lock = SPINLOCK_INIT;
struct process *process_table[PROCESS_TABLE_SIZE];

/* Performs ERET to EL0 using the trap frame on the kernel stack */
extern void ret_to_user(void);

/*
 * va_init - Resets a virtual-address allocator to its initial state.
 */
static void va_init(struct va_allocator *va)
{
    va->count = 0;
    for (size_t i = 0; i < USER_VA_MAX_REGIONS; i++) {
        va->regions[i].base = 0;
        va->regions[i].pages = 0;
    }
}

/*
 * alloc_kernel_stack - Allocates a guarded kernel stack.
 */
static void *alloc_kernel_stack(void)
{
    unsigned char *base = (unsigned char *)pmm_alloc_pages(SCHED_STACK_PAGES);
    if (!base) {
        return NULL;
    }

    /* Guard page: unmap to trap stack underflow */
    mmu_unmap_page((unsigned long)base);

    /* Set canary at the bottom of the usable region */
    *(unsigned long *)(base + PAGE_SIZE) = SCHED_STACK_CANARY;

    return base + PAGE_SIZE;
}

/*
 * free_kernel_stack - Releases a kernel stack and remaps its guard page.
 */
static void free_kernel_stack(void *stack_base)
{
    if (!stack_base) {
        return;
    }
    void *alloc_base = (void *)((uintptr_t)stack_base - PAGE_SIZE);

    /* Remap guard page so PMM can zero the memory safely */
    mmu_map_page((unsigned long)alloc_base, V2P(alloc_base), MMU_FLAGS_KERNEL_RW);
    pmm_free_pages(alloc_base, SCHED_STACK_PAGES);
}

static void open_std_fds(uint32_t pid)
{
    vfs_open_pid("/dev/console", VFS_O_RDONLY, pid);
    vfs_open_pid("/dev/console", VFS_O_WRONLY, pid);
    vfs_open_pid("/dev/console", VFS_O_WRONLY, pid);
}

static void close_all_fds(struct process *p)
{
    unsigned long fdflags = spin_lock_irqsave(&p->fd_lock);
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        struct vfs_file *f = p->fd_table[i];
        if (!f) {
            continue;
        }

        p->fd_table[i] = NULL;
        vfs_file_put(f);
    }
    spin_unlock_irqrestore(&p->fd_lock, fdflags);
}

/*
 * build_trap_frame - Sets initial register state for a return to user-space.
 */
static struct exception_trap_frame *build_trap_frame(uintptr_t kstack_base, uint64_t entry_pc,
                                                     uintptr_t user_sp_top)
{
    uintptr_t kernel_stack_top = kstack_base + SCHED_TASK_STACK_SIZE;
    uintptr_t tf_addr = (kernel_stack_top - sizeof(struct exception_trap_frame)) & ~15UL;
    struct exception_trap_frame *tf = (struct exception_trap_frame *)tf_addr;

    memset(tf, 0, sizeof(*tf));
    tf->elr_el1 = entry_pc;
    tf->spsr_el1 = SPSR_EL0_USER;
    tf->sp_el0 = user_sp_top & ~15UL;

    return tf;
}

static uintptr_t setup_user_stack(struct va_allocator *va, unsigned long *pgd, size_t pages)
{
    uintptr_t vbase = process_va_alloc(va, pages);
    if (!vbase) {
        return 0;
    }

    for (size_t i = 0; i < pages; i++) {
        void *page = pmm_alloc_page();
        if (!page) {
            PANIC("process: user stack OOM");
        }
        mmu_user_map_page(pgd, vbase + i * PAGE_SIZE, V2P(page), MMU_PAGE_USER_DATA);
    }
    return vbase;
}

/*
 * stack_copy_in - Copies kernel data into a not-yet-active user address space.
 *
 * The freshly-allocated destination pages are not physically contiguous, so a
 * single memcpy that crosses a page boundary would run off one frame into an
 * unrelated one. Resolve and copy one page at a time via the target pgd.
 */
static void stack_copy_in(unsigned long *pgd, uintptr_t user_dst, const void *src, size_t len)
{
    const uint8_t *s = (const uint8_t *)src;

    while (len > 0) {
        unsigned long paddr;
        if (!mmu_user_query(pgd, user_dst & ~0xFFFUL, &paddr, NULL)) {
            return;
        }

        size_t page_off = (size_t)(user_dst & 0xFFFUL);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > len) {
            chunk = len;
        }

        memcpy((void *)(P2V(paddr) + page_off), s, chunk);
        user_dst += chunk;
        s += chunk;
        len -= chunk;
    }
}

void process_flush_icache_range(void *start, size_t size)
{
    unsigned long addr = (unsigned long)start & ~63UL;
    unsigned long end = ((unsigned long)start + size + 63UL) & ~63UL;

    for (unsigned long a = addr; a < end; a += 64) {
        asm volatile("dc cvau, %0" : : "r"(a) : "memory");
    }
    asm volatile("dsb ish" : : : "memory");

    for (unsigned long a = addr; a < end; a += 64) {
        asm volatile("ic ivau, %0" : : "r"(a) : "memory");
    }
    asm volatile("dsb ish" : : : "memory");
    asm volatile("isb");
}

uintptr_t process_va_alloc(struct va_allocator *va, size_t pages)
{
    if (pages == 0 || va->count >= USER_VA_MAX_REGIONS) {
        return 0;
    }

    size_t size = pages * PAGE_SIZE;
    if (size / PAGE_SIZE != pages) {
        return 0;
    }

    /*
     * Take the lowest gap the request fits in. A bump pointer would be simpler,
     * but the space a released region occupied would then be gone for the life
     * of the process -- even the range a failed mmap briefly reserved.
     */
    uintptr_t candidate = USER_VA_BASE;
    size_t slot = va->count;

    for (size_t i = 0; i < va->count; i++) {
        uintptr_t region_base = va->regions[i].base;

        if (candidate + size <= region_base) {
            slot = i;
            break;
        }

        uintptr_t region_end = region_base + va->regions[i].pages * PAGE_SIZE;
        if (region_end > candidate) {
            candidate = region_end;
        }
    }

    if (candidate + size < candidate || candidate + size > USER_VA_LIMIT) {
        return 0;
    }

    for (size_t j = va->count; j > slot; j--) {
        va->regions[j] = va->regions[j - 1];
    }
    va->regions[slot].base = candidate;
    va->regions[slot].pages = pages;
    va->count++;

    return candidate;
}

void process_va_free(struct va_allocator *va, uintptr_t base)
{
    for (size_t i = 0; i < va->count; i++) {
        if (va->regions[i].base != base) {
            continue;
        }
        for (size_t j = i; j + 1 < va->count; j++) {
            va->regions[j] = va->regions[j + 1];
        }
        va->count--;
        return;
    }
}

int process_find_current(void)
{
    struct task *t = sched_get_current();
    return t ? (int)t->pid : -PERS_ERR_NO_SUCH_PROCESS;
}

struct process *process_current(void)
{
    struct task *t = sched_get_current();
    return t ? process_slot(t->pid) : NULL;
}

unsigned long process_get_ttbr0(uint32_t pid)
{
    unsigned long flags = spin_lock_irqsave(&process_table_lock);

    struct process *p = process_slot(pid);
    if (!p || p->state == PROCESS_STATE_DEAD) {
        spin_unlock_irqrestore(&process_table_lock, flags);
        return mmu_kernel_ttbr0();
    }

    unsigned long ttbr0 = p->ttbr0;
    spin_unlock_irqrestore(&process_table_lock, flags);
    return ttbr0;
}

/*
 * process_alloc_pcb - Allocates an initialised, unpublished PCB.
 */
static struct process *process_alloc_pcb(uint32_t pid)
{
    struct process *p = heap_malloc(sizeof(struct process));
    if (!p) {
        return NULL;
    }

    memset(p, 0, sizeof(*p));
    p->pid = pid;
    p->pgid = pid;
    p->sid = pid;
    p->has_execed = 0;
    p->state = PROCESS_STATE_RUNNING;
    p->fd_lock = (spinlock_t)SPINLOCK_INIT;
    return p;
}

/*
 * process_release_slot - Frees a slot's PCB and marks the slot free.
 */
static void process_release_slot(uint32_t pid)
{
    if (pid >= PROCESS_TABLE_SIZE) {
        return;
    }

    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    struct process *p = process_table[pid];
    process_table[pid] = NULL;
    spin_unlock_irqrestore(&process_table_lock, flags);

    heap_free(p);
}

void process_init(void)
{
    for (size_t i = 0; i < PROCESS_TABLE_SIZE; i++) {
        process_table[i] = NULL;
    }

    /* Slot 0 is the kernel itself: always present, never reaped. */
    struct process *kernel = process_alloc_pcb(0);
    if (!kernel) {
        PANIC("proc: cannot allocate the kernel PCB");
    }
    strcpy(kernel->name, "kernel");
    process_table[0] = kernel;

    pr_info("proc: initialized (%zu slots, %zu bytes per process)\n", (size_t)PROCESS_TABLE_SIZE,
            sizeof(struct process));
}

void process_create(void *code_ptr, size_t code_size, uint32_t pid)
{
    if (pid >= PROCESS_TABLE_SIZE || code_size == 0 || code_size > PAGE_SIZE) {
        return;
    }

    /* Allocated before the lock: heap_malloc must not run with interrupts off. */
    struct process *p = process_alloc_pcb(pid);
    if (!p) {
        return;
    }

    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    if (process_table[pid]) {
        spin_unlock_irqrestore(&process_table_lock, flags);
        heap_free(p);
        return;
    }
    process_table[pid] = p;
    spin_unlock_irqrestore(&process_table_lock, flags);

    va_init(&p->va);

    unsigned long *user_pgd = mmu_create_user_pgd();
    if (!user_pgd) {
        PANIC("process_create: failed to create user PGD");
    }

    void *code_page = pmm_alloc_page();
    if (!code_page) {
        PANIC("process_create: OOM for code page");
    }

    uintptr_t vaddr_code = process_va_alloc(&p->va, 1);
    if (!vaddr_code) {
        PANIC("process_create: VA space exhausted");
    }

    mmu_user_map_page(user_pgd, vaddr_code, V2P(code_page), MMU_PAGE_USER_CODE);
    memcpy(code_page, code_ptr, code_size);
    process_flush_icache_range(code_page, code_size);
    asm volatile("ic ialluis\n dsb ish\n isb");

    uintptr_t vaddr_user_stack = setup_user_stack(&p->va, user_pgd, PROCESS_USER_STACK_PAGES);
    if (!vaddr_user_stack) {
        PANIC("process_create: user stack OOM");
    }

    void *kstack = alloc_kernel_stack();
    if (!kstack) {
        PANIC("process_create: kernel stack OOM");
    }

    p->parent_pid = 0;
    p->pid = pid;
    p->user_pgd = user_pgd;
    p->asid = 0;
    p->asid_generation = 0;
    p->ttbr0 = V2P(user_pgd);
    p->vaddr_code = vaddr_code;
    p->paddr_code = V2P(code_page);
    p->vaddr_user_stack = vaddr_user_stack;
    p->vaddr_kernel_stack = (uintptr_t)kstack;
    p->paddr_kernel_stack = V2P(kstack);
    strcpy(p->name, "init");

    uintptr_t user_sp_top = vaddr_user_stack + PROCESS_USER_STACK_PAGES * PAGE_SIZE;
    struct exception_trap_frame *tf =
        build_trap_frame((uintptr_t)kstack, (uint64_t)vaddr_code, user_sp_top);

    p->context.sp = (unsigned long)tf;
    p->context.lr = (unsigned long)ret_to_user;

    int err;
    p->cwd = vfs_resolve_path("/", NULL, &err);
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        p->fd_table[i] = NULL;
        p->fd_flags[i] = 0;
    }
    open_std_fds(pid);

    memset(p->signal_handlers, 0, sizeof(p->signal_handlers));
    for (int i = 0; i < SIGNAL_COUNT; i++) {
        p->signal_handlers[i].sa_handler = SIGNAL_DFL;
    }

    struct task *t =
        sched_create_user_task(p->context.sp, p->context.lr, p->vaddr_kernel_stack, pid);
    if (!t) {
        PANIC("process_create: task creation failed");
    }
    p->main_task = t;
    enqueue_ready(get_core_id(), t);
}

int process_create_from_file(const char *path, uint32_t pid)
{
    if (pid >= PROCESS_TABLE_SIZE) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    /* Allocated before the lock: heap_malloc must not run with interrupts off. */
    struct process *p = process_alloc_pcb(pid);
    if (!p) {
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    if (process_table[pid]) {
        spin_unlock_irqrestore(&process_table_lock, flags);
        heap_free(p);
        return -PERS_ERR_ALREADY_EXISTS;
    }
    process_table[pid] = p;
    spin_unlock_irqrestore(&process_table_lock, flags);

    va_init(&p->va);

    unsigned long *user_pgd = mmu_create_user_pgd();
    if (!user_pgd) {
        process_release_slot(pid);
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    uint64_t entry_point;
    if (elf_load(path, user_pgd, &entry_point) != 0) {
        mmu_destroy_user_pgd(user_pgd);
        process_release_slot(pid);
        return -PERS_ERR_EXECUTABLE_FORMAT_ERROR;
    }

    uintptr_t vaddr_stack = setup_user_stack(&p->va, user_pgd, PROCESS_USER_STACK_PAGES);
    void *kstack = alloc_kernel_stack();

    if (!vaddr_stack || !kstack) {
        mmu_destroy_user_pgd(user_pgd);
        process_release_slot(pid);
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    p->parent_pid = 0;
    p->pid = pid;
    p->user_pgd = user_pgd;
    p->asid = 0;
    p->asid_generation = 0;
    p->ttbr0 = V2P(user_pgd);
    p->vaddr_code = (uintptr_t)entry_point;
    p->vaddr_user_stack = vaddr_stack;
    p->vaddr_kernel_stack = (uintptr_t)kstack;
    p->paddr_kernel_stack = V2P(kstack);

    const char *last_slash = strrchr(path, '/');
    const char *filename = last_slash ? last_slash + 1 : path;
    strncpy(p->name, filename, sizeof(p->name) - 1);
    p->name[sizeof(p->name) - 1] = '\0';

    uintptr_t user_sp_top = vaddr_stack + PROCESS_USER_STACK_PAGES * PAGE_SIZE;
    struct exception_trap_frame *tf = build_trap_frame((uintptr_t)kstack, entry_point, user_sp_top);

    p->context.sp = (unsigned long)tf;
    p->context.lr = (unsigned long)ret_to_user;

    int err;
    p->cwd = vfs_resolve_path("/", NULL, &err);
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        p->fd_table[i] = NULL;
        p->fd_flags[i] = 0;
    }
    open_std_fds(pid);

    memset(p->signal_handlers, 0, sizeof(p->signal_handlers));
    for (int i = 0; i < SIGNAL_COUNT; i++) {
        p->signal_handlers[i].sa_handler = SIGNAL_DFL;
    }

    struct task *t =
        sched_create_user_task(p->context.sp, p->context.lr, p->vaddr_kernel_stack, pid);
    if (!t) {
        close_all_fds(p);
        if (p->cwd) {
            vfs_vnode_put(p->cwd);
        }
        free_kernel_stack(kstack);
        mmu_destroy_user_pgd(user_pgd);
        process_release_slot(pid);
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    p->main_task = t;
    enqueue_ready(get_core_id(), t);

    pr_info("proc: loaded '%s' (PID %u)\n", path, pid);
    return PERS_SUCCESS;
}

/* Limits on a single exec vector: entries, and bytes per entry. */
#define EXEC_MAX_VECTOR 128
#define EXEC_MAX_ARG    1024

static void free_vector(char **vec, int count)
{
    for (int i = 0; i < count; i++) {
        heap_free(vec[i]);
    }
}

/*
 * copy_user_vector - Copies a NULL-terminated user argv/envp into kernel memory.
 *
 * Stops at the first NULL entry. *out_count excludes the terminator, and out[]
 * is left NULL-terminated so it can be walked directly.
 */
static int copy_user_vector(char *const user_vec[], char **out, int *out_count)
{
    int count = 0;
    *out_count = 0;
    out[0] = NULL;

    if (!user_vec) {
        return PERS_SUCCESS;
    }

    while (count < EXEC_MAX_VECTOR - 1) {
        char *uentry;
        if (copy_from_user(&uentry, &user_vec[count], sizeof(char *)) != 0) {
            return -PERS_ERR_INVALID_ARGUMENT;
        }
        if (!uentry) {
            break;
        }

        char *kentry = heap_malloc(EXEC_MAX_ARG);
        if (!kentry) {
            return -PERS_ERR_OUT_OF_MEMORY;
        }

        long len = strncpy_from_user(kentry, uentry, EXEC_MAX_ARG);
        if (len < 0) {
            heap_free(kentry);
            return (int)len;
        }

        out[count++] = kentry;
        *out_count = count;
    }

    out[count] = NULL;
    return PERS_SUCCESS;
}

int process_exec(const char *path, char *const argv[], char *const envp[])
{
    int pid = process_find_current();
    if (pid < 0) {
        return pid;
    }

    struct process *p = process_slot((uint32_t)pid);
    if (!p) {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }

    int fds_to_close[VFS_MAX_FDS];
    int close_count = 0;

    unsigned long fdflags = spin_lock_irqsave(&p->fd_lock);
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (p->fd_table[i] && (p->fd_flags[i] & VFS_FD_CLOEXEC)) {
            fds_to_close[close_count++] = i;
        }
    }
    spin_unlock_irqrestore(&p->fd_lock, fdflags);

    for (int i = 0; i < close_count; i++) {
        vfs_close(fds_to_close[i]);
    }

    /*
     * Copy the vectors in before switching address space. A NULL entry ends the
     * vector, but an oversized or unreadable string is a hard failure: silently
     * truncating would hand the new image a different argv than it asked for.
     */
    char *kargv[EXEC_MAX_VECTOR];
    char *kenvp[EXEC_MAX_VECTOR];
    int argc = 0;
    int envc = 0;

    int copy_err = copy_user_vector(argv, kargv, &argc);
    if (copy_err != PERS_SUCCESS) {
        free_vector(kargv, argc);
        return copy_err;
    }

    copy_err = copy_user_vector(envp, kenvp, &envc);
    if (copy_err != PERS_SUCCESS) {
        free_vector(kargv, argc);
        free_vector(kenvp, envc);
        return copy_err;
    }

    unsigned long *new_pgd = mmu_create_user_pgd();
    uint64_t entry_point;
    if (!new_pgd || elf_load(path, new_pgd, &entry_point) != 0) {
        mmu_destroy_user_pgd(new_pgd);
        free_vector(kargv, argc);
        free_vector(kenvp, envc);
        return -PERS_ERR_EXECUTABLE_FORMAT_ERROR;
    }

    struct va_allocator new_va;
    va_init(&new_va);
    uintptr_t new_stack_base = setup_user_stack(&new_va, new_pgd, PROCESS_USER_STACK_PAGES);
    if (!new_stack_base) {
        mmu_destroy_user_pgd(new_pgd);
        free_vector(kargv, argc);
        free_vector(kenvp, envc);
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    /* Set up user stack with argc/argv (top-down) */
    uintptr_t user_sp = new_stack_base + PROCESS_USER_STACK_PAGES * PAGE_SIZE;
    uintptr_t karg_user_vaddrs[128];

    for (int i = 0; i < argc; i++) {
        size_t len = strlen(kargv[i]) + 1;
        user_sp = (user_sp - len) & ~7UL;
        stack_copy_in(new_pgd, user_sp, kargv[i], len);
        karg_user_vaddrs[i] = user_sp;
    }

    user_sp = (user_sp - (argc + 1) * sizeof(uintptr_t)) & ~15UL;
    uintptr_t argv_ptr = user_sp;

    for (int i = 0; i < argc; i++) {
        unsigned long paddr;
        if (mmu_user_query(new_pgd, (user_sp + i * 8) & ~0xFFFUL, &paddr, NULL)) {
            *(uintptr_t *)(P2V(paddr) + ((user_sp + i * 8) & 0xFFF)) = karg_user_vaddrs[i];
        }
    }
    unsigned long paddr_null;
    if (mmu_user_query(new_pgd, (user_sp + argc * 8) & ~0xFFFUL, &paddr_null, NULL)) {
        *(uintptr_t *)(P2V(paddr_null) + ((user_sp + argc * 8) & 0xFFF)) = 0;
    }

    free_vector(kargv, argc);

    /* Set up user stack with envp (top-down) */
    uintptr_t kenv_user_vaddrs[128];

    for (int i = 0; i < envc; i++) {
        size_t len = strlen(kenvp[i]) + 1;
        user_sp = (user_sp - len) & ~7UL;
        stack_copy_in(new_pgd, user_sp, kenvp[i], len);
        kenv_user_vaddrs[i] = user_sp;
    }

    user_sp = (user_sp - (envc + 1) * sizeof(uintptr_t)) & ~15UL;
    uintptr_t envp_ptr = user_sp;

    for (int i = 0; i < envc; i++) {
        unsigned long paddr;
        if (mmu_user_query(new_pgd, (user_sp + i * 8) & ~0xFFFUL, &paddr, NULL)) {
            *(uintptr_t *)(P2V(paddr) + ((user_sp + i * 8) & 0xFFF)) = kenv_user_vaddrs[i];
        }
    }
    unsigned long paddr_envp_null;
    if (mmu_user_query(new_pgd, (user_sp + envc * 8) & ~0xFFFUL, &paddr_envp_null, NULL)) {
        *(uintptr_t *)(P2V(paddr_envp_null) + ((user_sp + envc * 8) & 0xFFF)) = 0;
    }

    free_vector(kenvp, envc);
    close_all_fds(p);
    open_std_fds((uint32_t)pid);

    for (int i = 0; i < SIGNAL_COUNT; i++) {
        if (p->signal_handlers[i].sa_handler != SIGNAL_IGN) {
            memset(&p->signal_handlers[i], 0, sizeof(struct sigaction));
            p->signal_handlers[i].sa_handler = SIGNAL_DFL;
        }
    }
    p->pending_signals = 0;
    p->default_sigrestorer = 0;

    unsigned long *old_pgd = p->user_pgd;
    p->user_pgd = new_pgd;
    p->vaddr_code = (uintptr_t)entry_point;
    p->vaddr_user_stack = new_stack_base;
    p->va = new_va;
    p->ttbr0 = V2P(new_pgd) | asid_ttbr_field(p->asid);

    const char *last_slash = strrchr(path, '/');
    const char *filename = last_slash ? last_slash + 1 : path;
    strncpy(p->name, filename, sizeof(p->name) - 1);
    p->name[sizeof(p->name) - 1] = '\0';

    mmu_switch_user(new_pgd, p->asid);
    unsigned long asid_field = asid_ttbr_field(p->asid);
    asm volatile("dsb ish" ::: "memory");
    asm volatile("tlbi aside1is, %0" : : "r"(asid_field));
    asm volatile("dsb ish" ::: "memory");
    asm volatile("isb");

    if (old_pgd) {
        mmu_destroy_user_pgd(old_pgd);
    }

    struct exception_trap_frame *tf = build_trap_frame(p->vaddr_kernel_stack, entry_point, user_sp);
    tf->x[0] = (uint64_t)argc;
    tf->x[1] = (uint64_t)argv_ptr;
    tf->x[2] = (uint64_t)envp_ptr;

    p->context.sp = (unsigned long)tf;
    p->context.lr = (unsigned long)ret_to_user;

    struct task *curr = sched_get_current();
    if (curr) {
        curr->context.sp = (unsigned long)tf;
        curr->context.lr = (unsigned long)ret_to_user;
        curr->ttbr0 = p->ttbr0;
        curr->skip_signals = 1;
    }

    p->has_execed = 1;
    pr_info("proc: PID %d exec '%s'\n", pid, path);
    return PERS_SUCCESS;
}

void process_exit(uint32_t pid, int exit_status)
{
    struct process *p = process_slot(pid);
    if (!p) {
        return;
    }

    process_state_t expected = PROCESS_STATE_RUNNING;
    if (!__atomic_compare_exchange_n(&p->state, &expected, PROCESS_STATE_DEAD, 0, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST)) {
        return;
    }

    if (p->sid == p->pid) {
        tty_session_exit(p->sid);
    }

    pr_info("proc: PID %u exiting with status %d\n", pid, exit_status);

    /* Reparent orphaned processes to init (PID 1) */
    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    for (int i = 1; i < PROCESS_TABLE_SIZE; i++) {
        if (i == (int)pid) {
            continue;
        }
        if (!process_table[i]) {
            continue;
        }
        if (process_table[i]->parent_pid == pid) {
            process_table[i]->parent_pid = 1;
        }
    }
    spin_unlock_irqrestore(&process_table_lock, flags);

    close_all_fds(p);
    if (p->cwd) {
        vfs_vnode_put(p->cwd);
        p->cwd = NULL;
    }

    unsigned long *pgd = p->user_pgd;
    p->user_pgd = NULL;
    if (pgd) {
        /*
         * Order matters here. Leave the address space first: TTBR0 still names
         * this pgd, and mmu_destroy_user_pgd hands every one of its pages back
         * to the allocator, where another core can immediately reuse them.
         * Release the ASID next -- it also broadcasts the TLB invalidate -- so
         * it cannot be recycled to a new process while a stale base is loaded.
         */
        mmu_leave_user();
        asid_free(&p->asid, &p->asid_generation);
        mmu_destroy_user_pgd(pgd);
    }

    p->exit_status = exit_status;

    flags = spin_lock_irqsave(&process_table_lock);
    p->va.count = 0;
    p->state = PROCESS_STATE_ZOMBIE;

    /* cleanup_dead_task frees this task shortly, but the slot lives on until a
     * parent reaps it. Drop the pointer with the same store that publishes the
     * zombie state, so nothing can find it in between. */
    p->main_task = NULL;

    uint32_t ppid = p->parent_pid;
    struct process *parent = process_slot(ppid);
    int notify_parent = (ppid != 0 && parent && parent->state == PROCESS_STATE_RUNNING);
    spin_unlock_irqrestore(&process_table_lock, flags);

    /* Notify the parent outside the lock (signal_send now takes it itself), and
     * wake a parent blocked in waitpid() even if it ignores SIGCHLD. Re-validate
     * under the lock before touching main_task in case the slot was reused. */
    if (notify_parent) {
        signal_send(ppid, SIGNAL_CHLD);

        flags = spin_lock_irqsave(&process_table_lock);
        parent = process_table[ppid];
        if (parent && parent->state == PROCESS_STATE_RUNNING && parent->main_task != NULL) {
            sched_unblock(parent->main_task);
        }
        spin_unlock_irqrestore(&process_table_lock, flags);
    }

    struct task *dying = sched_get_current();
    if (dying) {
        dying->state = SCHED_TASK_DEAD;
    }

    schedule();
    __builtin_unreachable();
}

/*
 * process_claim_slot - Reserves a free PID slot with a fresh PCB.
 */
static int process_claim_slot(void)
{
    /*
     * Build the PCB before publishing it. A half-initialised slot can no longer
     * be observed at all, rather than being merely brief: until the pointer is
     * stored, no other core can reach it.
     */
    struct process *p = process_alloc_pcb(0);
    if (!p) {
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    unsigned long flags = spin_lock_irqsave(&process_table_lock);

    for (int i = 1; i < PROCESS_TABLE_SIZE; i++) {
        if (process_table[i]) {
            continue;
        }

        p->pid = (uint32_t)i;
        process_table[i] = p;

        spin_unlock_irqrestore(&process_table_lock, flags);
        return i;
    }

    spin_unlock_irqrestore(&process_table_lock, flags);
    heap_free(p);
    return -PERS_ERR_OUT_OF_RESOURCES;
}

#ifdef CONFIG_TESTS
int process_test_claim_slot(void)
{
    return process_claim_slot();
}

void process_test_release_slot(uint32_t pid)
{
    process_release_slot(pid);
}
#endif

int process_fork(struct exception_trap_frame *parent_tf)
{
    int parent_pid = process_find_current();
    if (parent_pid < 0) {
        return parent_pid;
    }

    struct process *parent = process_slot((uint32_t)parent_pid);
    if (!parent) {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }

    int child_pid = process_claim_slot();
    if (child_pid < 0) {
        return child_pid;
    }

    struct process *child = process_table[child_pid];

    unsigned long *child_pgd = mmu_copy_user_pgd(parent->user_pgd);
    void *kstack = alloc_kernel_stack();

    if (!child_pgd || !kstack) {
        mmu_destroy_user_pgd(child_pgd);
        process_release_slot((uint32_t)child_pid);
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    child->user_pgd = child_pgd;
    child->asid = 0;
    child->asid_generation = 0;
    child->ttbr0 = V2P(child_pgd);
    child->vaddr_code = parent->vaddr_code;
    child->vaddr_user_stack = parent->vaddr_user_stack;
    child->vaddr_kernel_stack = (uintptr_t)kstack;
    child->paddr_kernel_stack = V2P(kstack);
    child->va = parent->va;
    child->parent_pid = (uint32_t)parent_pid;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->has_execed = 0;
    strncpy(child->name, parent->name, sizeof(child->name) - 1);
    child->name[sizeof(child->name) - 1] = '\0';

    memcpy(child->signal_handlers, parent->signal_handlers, sizeof(child->signal_handlers));
    child->pending_signals = 0;
    child->blocked_signals = parent->blocked_signals;
    child->default_sigrestorer = parent->default_sigrestorer;

    if (parent->cwd) {
        child->cwd = parent->cwd;
        atomic_inc(&child->cwd->refcount);
    }

    unsigned long fdflags = spin_lock_irqsave(&parent->fd_lock);
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (parent->fd_table[i]) {
            child->fd_table[i] = parent->fd_table[i];
            child->fd_flags[i] = parent->fd_flags[i];
            atomic_inc(&child->fd_table[i]->refcount);
        }
    }
    spin_unlock_irqrestore(&parent->fd_lock, fdflags);

    uintptr_t kernel_stack_top = (uintptr_t)kstack + SCHED_TASK_STACK_SIZE;
    uintptr_t tf_addr = (kernel_stack_top - sizeof(struct exception_trap_frame)) & ~15UL;
    struct exception_trap_frame *child_tf = (struct exception_trap_frame *)tf_addr;

    memcpy(child_tf, parent_tf, sizeof(*child_tf));
    child_tf->x[0] = 0; /* Child returns 0 from fork */

    child->context.sp = (unsigned long)child_tf;
    child->context.lr = (unsigned long)ret_to_user;

    struct task *t = sched_create_user_task(child->context.sp, child->context.lr, (uintptr_t)kstack,
                                            (uint32_t)child_pid);
    if (!t) {
        close_all_fds(child);
        if (child->cwd) {
            vfs_vnode_put(child->cwd);
        }
        free_kernel_stack(kstack);
        mmu_destroy_user_pgd(child_pgd);
        process_release_slot((uint32_t)child_pid);
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    child->main_task = t;
    enqueue_ready(get_core_id(), t);

    pr_info("proc: PID %d forked -> PID %d\n", parent_pid, child_pid);
    return child_pid;
}

int process_waitpid(int pid, int *status, int options)
{
    int parent_pid = process_find_current();
    if (parent_pid < 0) {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }

    for (;;) {
        unsigned long irqf = irq_save();
        spin_lock(&process_table_lock);

        int has_children = 0;
        for (int i = 1; i < PROCESS_TABLE_SIZE; i++) {
            struct process *candidate = process_table[i];

            if (!candidate) {
                continue;
            }
            if (candidate->parent_pid != (uint32_t)parent_pid) {
                continue;
            }
            if (pid != -1 && (int)candidate->pid != pid) {
                continue;
            }

            has_children = 1;

            if (candidate->state == PROCESS_STATE_ZOMBIE) {
                int found_pid = (int)candidate->pid;
                if (status) {
                    *status = candidate->exit_status;
                }
                process_table[i] = NULL;
                spin_unlock(&process_table_lock);
                irq_restore(irqf);
                heap_free(candidate);
                return found_pid;
            }

            /* Reported once per stop, so a caller that waits again blocks
             * instead of spinning on a child that is still stopped. */
            if ((options & WUNTRACED) && !candidate->stop_reported && candidate->main_task
                && candidate->main_task->state == SCHED_TASK_STOPPED) {
                int found_pid = (int)candidate->pid;
                candidate->stop_reported = 1;
                if (status) {
                    *status = PERS_STATUS_STOPPED | SIGNAL_TSTP;
                }
                spin_unlock(&process_table_lock);
                irq_restore(irqf);
                return found_pid;
            }
        }

        if (!has_children) {
            spin_unlock(&process_table_lock);
            irq_restore(irqf);
            return -PERS_ERR_NO_SUCH_PROCESS;
        }

        if (options & WNOHANG) {
            spin_unlock(&process_table_lock);
            irq_restore(irqf);
            return 0;
        }

        struct task *curr = sched_get_current();
        if (curr) {
            curr->state = SCHED_TASK_BLOCKED;
        }

        spin_unlock(&process_table_lock);
        schedule();
        irq_restore(irqf);
    }
}

void process_drop_to_user(void *code_vaddr, void *stack_vaddr)
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
