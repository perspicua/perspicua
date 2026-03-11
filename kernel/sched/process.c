#include "process.h"
#include "pmm.h"
#include "mmu.h"
#include "vfs.h"
#include "elf.h"
#include "string.h"
#include "stdio.h"
#include "panic.h"
#include "addr.h"
#include "arch/exception.h"

extern void ret_to_user(void);

void flush_icache_range(void* start, size_t size)
{
    unsigned long addr = (unsigned long)start & ~63UL;
    unsigned long end = (unsigned long)start + size;

    for (unsigned long a = addr; a < end; a += 64)
        asm volatile("dc cvau, %0" : : "r"(a));
    asm volatile("dsb ish");

    for (unsigned long a = addr; a < end; a += 64)
        asm volatile("ic ivau, %0" : : "r"(a));
    asm volatile("dsb ish");
    asm volatile("isb");
}

struct process process_table[PROCESS_TABLE_SIZE];

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

uintptr_t process_va_alloc(struct va_allocator* va, size_t pages)
{
    ASSERT(pages > 0);
    ASSERT(va->count < USER_VA_MAX_REGIONS);

    uintptr_t base = va->next_va;
    size_t size = pages * PAGE_SIZE;

    ASSERT(base + size > base);             // overflow check
    ASSERT(base + size <= 0x8000000000ULL); // 39-bit user VA limit

    va->next_va = base + size;
    va->regions[va->count].base = base;
    va->regions[va->count].pages = pages;
    va->count++;

    return base;
}

void process_va_free(struct va_allocator* va, uintptr_t base)
{
    for (size_t i = 0; i < va->count; i++)
    {
        if (va->regions[i].base == base)
        {
            for (size_t j = i; j + 1 < va->count; j++)
                va->regions[j] = va->regions[j + 1];
            va->count--;
            return;
        }
    }
}

int process_find_current(void)
{
    struct task* t = sched_get_current();
    if (!t)
        return -1;

    return (int)t->pid;
}

void process_init(void)
{
    for (size_t i = 0; i < PROCESS_TABLE_SIZE; i++)
    {
        process_table[i].state = PROCESS_STATE_EMPTY;
        process_table[i].fd_lock = (spinlock_t)SPINLOCK_INIT;
        for (size_t j = 0; j < MAX_FDS; j++)
            process_table[i].fd_table[j] = NULL;
    }

    // PID 0 is the kernel/boot process
    process_table[0].pid = 0;
    process_table[0].state = PROCESS_STATE_RUNNING;
    process_table[0].user_pgd = 0;
    process_table[0].asid = 0;
}

static void* alloc_kernel_stack(void)
{
    // 3 pages: 1 guard + 2 usable
    unsigned char* stack = (unsigned char*)pmm_alloc_pages(3);
    mmu_unmap_page((unsigned long)stack);
    // Write canary to bottom of usable part
    *(unsigned long*)(stack + PAGE_SIZE) = 0xDEADC0DEDEADC0DEULL;
    return stack;
}

void process_create(void* code_ptr, size_t code_size, uint32_t pid)
{
    if (pid >= PROCESS_TABLE_SIZE)
    {
        printf("[PROCESS] Error: PID %d out of range\n", pid);
        return;
    }
    if (process_table[pid].state != PROCESS_STATE_EMPTY)
    {
        printf("[PROCESS] Error: PID %d already in use\n", pid);
        return;
    }

    va_init(&process_table[pid].va);

    void* code_page = pmm_alloc_page();
    if (!code_page)
        PANIC("Out of memory for process code");
    void* stack_page = pmm_alloc_page();
    if (!stack_page)
        PANIC("Out of memory for process stack");

    process_table[pid].vaddr_code = process_va_alloc(&process_table[pid].va, 1);
    if (!process_table[pid].vaddr_code)
        PANIC("Out of virtual address space for process code");
    process_table[pid].vaddr_user_stack = process_va_alloc(&process_table[pid].va, 1);
    if (!process_table[pid].vaddr_user_stack)
        PANIC("Out of virtual address space for process stack");

    process_table[pid].paddr_code = V2P(code_page);
    process_table[pid].paddr_user_stack = V2P(stack_page);

    unsigned long* user_pgd = mmu_create_user_pgd();
    if (!user_pgd)
        PANIC("Failed to create user PGD for process");

    process_table[pid].user_pgd = user_pgd;
    process_table[pid].asid = pid;

    mmu_user_map_page(user_pgd, process_table[pid].vaddr_code, process_table[pid].paddr_code, PAGE_USER_CODE);
    mmu_user_map_page(user_pgd, process_table[pid].vaddr_user_stack, process_table[pid].paddr_user_stack,
                      PAGE_USER_DATA);

    void* kstack = alloc_kernel_stack();
    process_table[pid].vaddr_kernel_stack = (uintptr_t)kstack;
    process_table[pid].paddr_kernel_stack = V2P(kstack);

    memcpy(code_page, code_ptr, code_size);
    flush_icache_range(code_page, code_size);

    asm volatile("ic ialluis\n dsb ish\n isb");

    uintptr_t kernel_stack_top = process_table[pid].vaddr_kernel_stack + 3 * PAGE_SIZE;
    struct trap_frame* tf = (struct trap_frame*)(kernel_stack_top - sizeof(struct trap_frame));
    memset(tf, 0, sizeof(struct trap_frame));
    tf->elr_el1 = process_table[pid].vaddr_code;
    tf->spsr_el1 = 0x340;
    tf->sp_el0 = (process_table[pid].vaddr_user_stack + PAGE_SIZE) & ~15UL;

    process_table[pid].context.sp = (unsigned long)tf;
    process_table[pid].context.lr = (unsigned long)ret_to_user;

    process_table[pid].pid = pid;
    process_table[pid].state = PROCESS_STATE_RUNNING;

    for (size_t i = 0; i < MAX_FDS; i++)
        process_table[pid].fd_table[i] = NULL;
    vfs_open_pid("/dev/uart", O_RDONLY, pid);
    vfs_open_pid("/dev/uart", O_WRONLY, pid);
    vfs_open_pid("/dev/uart", O_WRONLY, pid);

    sched_create_user_task(process_table[pid].context.sp, process_table[pid].context.lr, process_table[pid].pid);
}

int process_create_from_file(const char* path, uint32_t pid)
{
    if (pid >= PROCESS_TABLE_SIZE)
        return -1;
    if (process_table[pid].state != PROCESS_STATE_EMPTY)
        return -1;

    va_init(&process_table[pid].va);

    unsigned long* user_pgd = mmu_create_user_pgd();
    if (!user_pgd)
        return -1;

    uint64_t entry_point;
    if (elf_load(path, user_pgd, &entry_point) != 0)
    {
        mmu_destroy_user_pgd(user_pgd);
        return -1;
    }

    size_t stack_pages = 4;
    uintptr_t vaddr_stack = process_va_alloc(&process_table[pid].va, stack_pages);
    if (!vaddr_stack)
    {
        mmu_destroy_user_pgd(user_pgd);
        return -1;
    }
    for (size_t i = 0; i < stack_pages; i++)
    {
        void* page = pmm_alloc_page();
        if (!page)
            PANIC("Out of memory for user stack");
        mmu_user_map_page(user_pgd, vaddr_stack + i * PAGE_SIZE, V2P(page), PAGE_USER_DATA);
    }

    void* kstack = alloc_kernel_stack();
    if (!kstack)
        PANIC("Out of memory for kernel stack");

    process_table[pid].pid = pid;
    process_table[pid].state = PROCESS_STATE_RUNNING;
    process_table[pid].user_pgd = user_pgd;
    process_table[pid].asid = pid;
    process_table[pid].vaddr_code = entry_point;
    process_table[pid].vaddr_user_stack = vaddr_stack;
    process_table[pid].vaddr_kernel_stack = (uintptr_t)kstack;
    process_table[pid].paddr_kernel_stack = V2P(kstack);

    uintptr_t kernel_stack_top = (uintptr_t)kstack + 3 * PAGE_SIZE;
    struct trap_frame* tf = (struct trap_frame*)(kernel_stack_top - sizeof(struct trap_frame));
    memset(tf, 0, sizeof(struct trap_frame));

    tf->elr_el1 = entry_point;
    tf->spsr_el1 = 0x340;
    tf->sp_el0 = (vaddr_stack + (stack_pages * PAGE_SIZE)) & ~15UL;

    process_table[pid].context.sp = (unsigned long)tf;
    process_table[pid].context.lr = (unsigned long)ret_to_user;

    for (size_t i = 0; i < MAX_FDS; i++)
        process_table[pid].fd_table[i] = NULL;
    vfs_open_pid("/dev/uart", O_RDONLY, pid);
    vfs_open_pid("/dev/uart", O_WRONLY, pid);
    vfs_open_pid("/dev/uart", O_WRONLY, pid);

    sched_create_user_task(process_table[pid].context.sp, process_table[pid].context.lr, pid);

    printf("[PROCESS] Loaded ELF %s for PID %d, entry at 0x%lx\n", path, pid, entry_point);
    return 0;
}

int process_exec(const char* path)
{
    int pid = process_find_current();
    if (pid < 0)
        return -1;

    struct process* p = &process_table[pid];

    unsigned long* new_pgd = mmu_create_user_pgd();
    if (!new_pgd)
        return -1;

    uint64_t entry_point;
    if (elf_load(path, new_pgd, &entry_point) != 0)
    {
        mmu_destroy_user_pgd(new_pgd);
        return -1;
    }

    struct va_allocator new_va;
    va_init(&new_va);
    size_t stack_pages = 4;
    uintptr_t vaddr_stack = process_va_alloc(&new_va, stack_pages);
    for (size_t i = 0; i < stack_pages; i++)
    {
        void* page = pmm_alloc_page();
        if (!page)
            PANIC("Out of memory for exec stack");
        mmu_user_map_page(new_pgd, vaddr_stack + i * PAGE_SIZE, V2P(page), PAGE_USER_DATA);
    }

    unsigned long* old_pgd = p->user_pgd;

    p->user_pgd = new_pgd;
    p->vaddr_code = entry_point;
    p->vaddr_user_stack = vaddr_stack;
    p->va = new_va;

    mmu_switch_user(new_pgd, p->asid);

    struct task* curr_task = sched_get_current();
    if (curr_task)
    {
        curr_task->ttbr0 = V2P(new_pgd) | (p->asid << 48);
    }

    unsigned long aside = (p->asid << 48);
    asm volatile("dsb ishst");
    asm volatile("tlbi aside1is, %0" : : "r"(aside));
    asm volatile("dsb ish");
    asm volatile("isb");

    if (old_pgd)
    {
        mmu_destroy_user_pgd(old_pgd);
    }

    uintptr_t kernel_stack_top = p->vaddr_kernel_stack + 3 * PAGE_SIZE;
    struct trap_frame* tf = (struct trap_frame*)(kernel_stack_top - sizeof(struct trap_frame));

    memset(tf, 0, sizeof(struct trap_frame));
    tf->elr_el1 = entry_point;
    tf->spsr_el1 = 0x340; // EL0t, interrupts enabled
    tf->sp_el0 = (vaddr_stack + (stack_pages * PAGE_SIZE)) & ~15UL;

    return 0;
}

void process_exit(uint32_t pid)
{
    if (pid >= PROCESS_TABLE_SIZE || process_table[pid].state != PROCESS_STATE_RUNNING)
        return;

    printf("[PROCESS] PID %d exiting. Reclaiming user memory...\n", process_table[pid].pid);

    if (process_table[pid].user_pgd)
    {
        mmu_destroy_user_pgd(process_table[pid].user_pgd);
        process_table[pid].user_pgd = 0;
    }

    // Kernel stack freeing is deferred to the scheduler
    process_table[pid].va.count = 0;
    process_table[pid].state = PROCESS_STATE_DEAD;
}

unsigned long process_get_ttbr0(uint32_t pid)
{
    if (pid >= PROCESS_TABLE_SIZE)
        return mmu_kernel_ttbr0();
    if (process_table[pid].state == PROCESS_STATE_EMPTY || process_table[pid].state == PROCESS_STATE_DEAD)
        return mmu_kernel_ttbr0();
    return V2P(process_table[pid].user_pgd) | (process_table[pid].asid << 48);
}

void drop_to_user(void* code_vaddr, void* stack_vaddr)
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
