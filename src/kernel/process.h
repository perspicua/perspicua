#ifndef _PROCESS_H_
#define _PROCESS_H_

#include "../lib/types.h"
#include "sched.h"

typedef enum
{
    PROCESS_STATE_EMPTY = 0,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_DEAD
} process_state_t;

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

    struct cpu_context context;
};

extern struct process process_table[16];

void process_init(void);
void process_create(void* code_ptr, size_t code_size, uint32_t pid);
void process_exit(void);
void drop_to_user(void* code_vaddr, void* stack_vaddr);
#endif // _PROCESS_H_
