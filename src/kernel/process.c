#include "process.h"
#include "pmm.h"
#include "mmu.h"
#include "../lib/string.h"
#include "../lib/stdio.h"
#include "../lib/panic.h"
#include "../kernel/addr.h"
#include "../arch/exception.h"

#define PROGRAM_SIZE 0x1000000ULL
#define PROCESS_TABLE_SIZE 16

extern void ret_to_user(void);

struct process process_table[16];

void process_init(void)
{
    for (size_t i = 0; i < PROCESS_TABLE_SIZE; i++)
    {
        process_table[i].state = PROCESS_STATE_EMPTY;
    }
}

void process_create(void* code_ptr, size_t code_size, uint32_t pid)
{
    if (process_table[pid].state != PROCESS_STATE_EMPTY)
    {
        PANIC("Only one process supported right now!\n");
    }

    void* code_page = pmm_alloc_page();
    void* stack_page = pmm_alloc_page();

    // TODO: these hardcoded addresses are shit! fix them
    process_table[pid].vaddr_code = 0x100000000ULL + (pid * PROGRAM_SIZE);
    process_table[pid].vaddr_user_stack = 0x100200000ULL + (pid * PROGRAM_SIZE);

    process_table[pid].paddr_code = V2P(code_page);
    process_table[pid].paddr_user_stack = V2P(stack_page);

    mmu_map_page(process_table[pid].vaddr_code, process_table[pid].paddr_code, PAGE_USER_CODE);
    mmu_map_page(process_table[pid].vaddr_user_stack, process_table[pid].paddr_user_stack, PAGE_USER_DATA);

    process_table[pid].vaddr_kernel_stack = (uintptr_t)pmm_alloc_page();
    process_table[pid].paddr_kernel_stack = V2P(process_table[pid].vaddr_kernel_stack);

    memcpy(code_page, code_ptr, code_size);

    uintptr_t kernel_stack_top = process_table[pid].vaddr_kernel_stack + 4096;
    struct trap_frame* tf = (struct trap_frame*)(kernel_stack_top - sizeof(struct trap_frame));
    memset(tf, 0, sizeof(struct trap_frame));
    tf->elr_el1 = 0x100000000ULL + (pid * PROGRAM_SIZE);
    tf->spsr_el1 = 0;
    tf->sp_el0 = 0x100200000ULL + 4096 + (pid * PROGRAM_SIZE);

    process_table[pid].context.sp = (unsigned long)tf;
    process_table[pid].context.lr = (unsigned long)ret_to_user;

    process_table[pid].pid = pid;
    process_table[pid].state = PROCESS_STATE_RUNNING;

    printf("[PROCESS] Created PID %d\n", process_table[pid].pid);
    sched_create_user_task(process_table[pid].context.sp, process_table[pid].context.lr);
}

void process_exit(void)
{
    uintptr_t elr;
    asm volatile("mrs %0, elr_el1" : "=r"(elr));
    uint32_t pid = (uint32_t)((elr - 0x100000000ULL) / PROGRAM_SIZE);

    if (pid >= PROCESS_TABLE_SIZE || process_table[pid].state != PROCESS_STATE_RUNNING)
        return;

    printf("[PROCESS] PID %d exiting. Reclaiming memory...\n", process_table[pid].pid);

    pmm_free_page((void*)P2V(process_table[pid].paddr_code));
    pmm_free_page((void*)P2V(process_table[pid].paddr_user_stack));

    pmm_free_page((void*)P2V(process_table[pid].paddr_kernel_stack));

    process_table[pid].state = PROCESS_STATE_DEAD;
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
