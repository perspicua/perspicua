#ifndef _PROCESS_H_
#define _PROCESS_H_

#include "types.h"
#include "vfs.h"
#include "arch/exception.h"
#include "sched.h"
#include "lock.h"

#define PROCESS_TABLE_SIZE 16
#define USER_VA_BASE 0x40000000ULL // 1GB - leaves lower area for ELF loading
#define USER_VA_MAX_REGIONS 16

#define SPSR_EL0_USER 0x00000000ULL
#define SPSR_EL1_KERN 0x000003C5ULL

#define STACK_CANARY_VALUE 0xDEADC0DEDEADC0DEULL
extern spinlock_t process_table_lock;

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
    unsigned long ttbr0;     // hardware-ready TTBR0 value (Phys PGD | ASID)

    uint32_t parent_pid;
    struct task* parent_task;
    int exit_status;

    struct cpu_context context;
    struct va_allocator va; // per-process user VA allocator

    struct file* fd_table[MAX_FDS];
    spinlock_t fd_lock;

    struct vnode* cwd;
};

extern struct process process_table[PROCESS_TABLE_SIZE];

void process_init(void);
void process_create(void* code_ptr, size_t code_size, uint32_t pid);
int process_create_from_file(const char* path, uint32_t pid);
int process_exec(const char* path);
void process_exit(uint32_t pid, int status);
void drop_to_user(void* code_vaddr, void* stack_vaddr);
int process_fork(struct trap_frame* parent_tf);
int process_waitpid(int pid, int* status);

// Returns the TTBR0 value for a given PID (0 if no such process).
unsigned long process_get_ttbr0(uint32_t pid);

// Per-process virtual address allocator
uintptr_t process_va_alloc(struct va_allocator* va, size_t pages);
void process_va_free(struct va_allocator* va, uintptr_t base);

void flush_icache_range(void* start, size_t size);

// Find PID of the user process running on this core. Returns -1 if none.
int process_find_current(void);

#endif // _PROCESS_H_
