#ifndef _PROCESS_H_
#define _PROCESS_H_

#include "../lib/types.h"
#include "sched.h"

#define PROCESS_TABLE_SIZE 16
#define USER_VA_BASE 0x100000ULL // skip first 1MB (null guard)
#define USER_VA_MAX_REGIONS 16

typedef enum
{
    PROCESS_STATE_EMPTY = 0,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_DEAD
} process_state_t;

struct va_region
{
    uintptr_t base;
    size_t pages;
};

struct va_allocator
{
    struct va_region regions[USER_VA_MAX_REGIONS];
    size_t count;
    uintptr_t next_va;
};

struct process
{
    uint32_t pid;
    process_state_t state;

    uintptr_t paddr_code;
    uintptr_t paddr_user_stack;

    uintptr_t vaddr_code;
    uintptr_t vaddr_user_stack;

    uintptr_t paddr_kernel_stack;
    uintptr_t vaddr_kernel_stack;

    unsigned long* user_pgd; // per-process TTBR0 page table
    unsigned long asid;      // address space ID for TLB tagging

    struct cpu_context context;
    struct va_allocator va; // per-process user VA allocator
};

extern struct process process_table[PROCESS_TABLE_SIZE];

void process_init(void);
void process_create(void* code_ptr, size_t code_size, uint32_t pid);
void process_exit(void);
void drop_to_user(void* code_vaddr, void* stack_vaddr);

// Returns the TTBR0 value for a given PID (0 if no such process).
unsigned long process_get_ttbr0(uint32_t pid);

// Per-process virtual address allocator
uintptr_t process_va_alloc(struct va_allocator* va, size_t pages);
void process_va_free(struct va_allocator* va, uintptr_t base);

// Find PID of the user process running on this core. Returns -1 if none.
int process_find_current(void);

#endif // _PROCESS_H_
