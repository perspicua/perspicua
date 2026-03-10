#include "kernel/process.h"
#include "kernel/pmm.h"
#include "kernel/mmu.h"
#include "kernel/vfs.h"
#include "kernel/elf.h"
#include "lib/string.h"
#include "lib/stdio.h"
#include "lib/panic.h"
#include "kernel/addr.h"
#include "arch/exception.h"

extern void ret_to_user(void);

// flush D-cache to Point of Unification and invalidate I-cache for a range.
// required after copying code into a page that will be executed —
// without this, real hardware (Cortex-A72) may execute stale I-cache contents.
void flush_icache_range(void* start, size_t size)
{
    // Cortex-A72 cache line = 64 bytes
    unsigned long addr = (unsigned long)start & ~63UL;
    unsigned long end = (unsigned long)start + size;

    // 1. clean D-cache to PoU so writes reach the level where I-cache reads
    for (unsigned long a = addr; a < end; a += 64)
        asm volatile("dc cvau, %0" : : "r"(a));
    asm volatile("dsb ish");

    // 2. invalidate I-cache so stale lines are discarded
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
    unsigned long ttbr0;
    asm volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0));
    unsigned long pgd_phys = ttbr0 & 0x0000FFFFFFFFF000ULL;

    if (pgd_phys == mmu_kernel_ttbr0())
        return 0;

    for (uint32_t i = 1; i < PROCESS_TABLE_SIZE; i++)
    {
        if (process_table[i].state == PROCESS_STATE_RUNNING && process_table[i].user_pgd &&
            V2P(process_table[i].user_pgd) == pgd_phys)
        {
            return (int)i;
        }
    }
    return -1;
}

void process_init(void)
{
    for (size_t i = 0; i < PROCESS_TABLE_SIZE; i++)
    {
        process_table[i].state = PROCESS_STATE_EMPTY;
        for (size_t j = 0; j < MAX_FDS; j++)
            process_table[i].fd_table[j] = NULL;
    }

    // PID 0 is the kernel/boot process
    process_table[0].pid = 0;
    process_table[0].state = PROCESS_STATE_RUNNING;
    process_table[0].user_pgd = 0;
    process_table[0].asid = 0;
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
    void* stack_page = pmm_alloc_page();

    process_table[pid].vaddr_code = process_va_alloc(&process_table[pid].va, 1);
    process_table[pid].vaddr_user_stack = process_va_alloc(&process_table[pid].va, 1);

    process_table[pid].paddr_code = V2P(code_page);
    process_table[pid].paddr_user_stack = V2P(stack_page);

    // create per-process user page table and map code + stack into it
    unsigned long* user_pgd = mmu_create_user_pgd();
    process_table[pid].user_pgd = user_pgd;
    process_table[pid].asid = pid; // simple: use PID as ASID

    mmu_user_map_page(user_pgd, process_table[pid].vaddr_code, process_table[pid].paddr_code, PAGE_USER_CODE);
    mmu_user_map_page(user_pgd, process_table[pid].vaddr_user_stack, process_table[pid].paddr_user_stack,
                      PAGE_USER_DATA);

    process_table[pid].vaddr_kernel_stack = (uintptr_t)pmm_alloc_page();
    process_table[pid].paddr_kernel_stack = V2P(process_table[pid].vaddr_kernel_stack);

    memcpy(code_page, code_ptr, code_size);
    flush_icache_range(code_page, code_size);

    asm volatile("ic ialluis\n dsb ish\n isb");

    uintptr_t kernel_stack_top = process_table[pid].vaddr_kernel_stack + 4096;
    struct trap_frame* tf = (struct trap_frame*)(kernel_stack_top - sizeof(struct trap_frame));
    memset(tf, 0, sizeof(struct trap_frame));
    tf->elr_el1 = process_table[pid].vaddr_code;
    tf->spsr_el1 = 0x340;
    tf->sp_el0 = process_table[pid].vaddr_user_stack + PAGE_SIZE;

    process_table[pid].context.sp = (unsigned long)tf;
    process_table[pid].context.lr = (unsigned long)ret_to_user;

    process_table[pid].pid = pid;
    process_table[pid].state = PROCESS_STATE_RUNNING;

    for (size_t i = 0; i < MAX_FDS; i++)
        process_table[pid].fd_table[i] = NULL;
    vfs_open_pid("/dev/uart", O_RDONLY, pid); // fd 0 : stdin
    vfs_open_pid("/dev/uart", O_WRONLY, pid); // fd 1 : stdout
    vfs_open_pid("/dev/uart", O_WRONLY, pid); // fd 2 : stderr

    printf("[PROCESS] Created PID %d (ASID %lu, TTBR0 0x%lx)\n", process_table[pid].pid, process_table[pid].asid,
           V2P(process_table[pid].user_pgd));
    printf("[PROCESS]   code  VA 0x%lx -> PA 0x%lx\n", process_table[pid].vaddr_code, process_table[pid].paddr_code);
    printf("[PROCESS]   stack VA 0x%lx -> PA 0x%lx\n", process_table[pid].vaddr_user_stack,
           process_table[pid].paddr_user_stack);
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
    uintptr_t first_stack_paddr = 0;
    for (size_t i = 0; i < stack_pages; i++)
    {
        void* page = pmm_alloc_page();
        if (!page)
            PANIC("Out of memory for user stack");
        if (i == 0)
            first_stack_paddr = V2P(page);
        mmu_user_map_page(user_pgd, vaddr_stack + i * PAGE_SIZE, V2P(page), PAGE_USER_DATA);
    }

    void* kstack = pmm_alloc_page();
    if (!kstack)
        PANIC("Out of memory for kernel stack");

    process_table[pid].pid = pid;
    process_table[pid].state = PROCESS_STATE_RUNNING;
    process_table[pid].user_pgd = user_pgd;
    process_table[pid].asid = pid;
    process_table[pid].vaddr_code = entry_point;
    process_table[pid].vaddr_user_stack = vaddr_stack;
    process_table[pid].paddr_user_stack = first_stack_paddr; // Store first stack page
    process_table[pid].paddr_code = 0;                       // ELF pages are multiple, tracked by PGD for now
    process_table[pid].vaddr_kernel_stack = (uintptr_t)kstack;
    process_table[pid].paddr_kernel_stack = V2P(kstack);

    uintptr_t kernel_stack_top = (uintptr_t)kstack + PAGE_SIZE;
    struct trap_frame* tf = (struct trap_frame*)(kernel_stack_top - sizeof(struct trap_frame));
    memset(tf, 0, sizeof(struct trap_frame));

    tf->elr_el1 = entry_point;
    tf->spsr_el1 = 0x340; // EL0t mode, interrupts enabled
    tf->sp_el0 = vaddr_stack + (stack_pages * PAGE_SIZE);

    process_table[pid].context.sp = (unsigned long)tf;
    process_table[pid].context.lr = (unsigned long)ret_to_user;

    for (size_t i = 0; i < MAX_FDS; i++)
        process_table[pid].fd_table[i] = NULL;
    vfs_open_pid("/dev/uart", O_RDONLY, pid); // fd 0 : stdin
    vfs_open_pid("/dev/uart", O_WRONLY, pid); // fd 1 : stdout
    vfs_open_pid("/dev/uart", O_WRONLY, pid); // fd 2 : stderr

    sched_create_user_task(process_table[pid].context.sp, process_table[pid].context.lr, pid);

    printf("[PROCESS] Loaded ELF %s for PID %d, entry at 0x%lx\n", path, pid, entry_point);
    return 0;
}

void process_exit(void)
{
    int found = process_find_current();
    if (found < 0)
        return;
    uint32_t pid = (uint32_t)found;

    if (pid >= PROCESS_TABLE_SIZE || process_table[pid].state != PROCESS_STATE_RUNNING)
        return;

    printf("[PROCESS] PID %d exiting. Reclaiming memory...\n", process_table[pid].pid);

    // switch TTBR0 to empty before destroying this process's page tables
    unsigned long ttbr0 = mmu_kernel_ttbr0();
    asm volatile("msr ttbr0_el1, %0\n isb" : : "r"(ttbr0));

    // mmu_destroy_user_pgd walks the page tables and frees all mapped
    // physical pages (ELF segments, stack, etc.) as well as table pages.
    if (process_table[pid].user_pgd)
    {
        mmu_destroy_user_pgd(process_table[pid].user_pgd);
        process_table[pid].user_pgd = 0;
    }

    if (process_table[pid].paddr_kernel_stack)
    {
        pmm_free_page((void*)P2V(process_table[pid].paddr_kernel_stack));
        process_table[pid].paddr_kernel_stack = 0;
    }

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
