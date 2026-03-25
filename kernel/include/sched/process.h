/*
 * process.h
 *
 * Core process management, lifecycle, and task definitions.
 * Handles user-space execution, process tracking, and address spaces.
 */

#ifndef PERSPICUA_KERNEL_PROCESS_H
#define PERSPICUA_KERNEL_PROCESS_H

#include "core/signals.h"
#include "types.h"
#include "fs/vfs.h"
#include "sched/sched.h"
#include "core/lock.h"
#include "arch/exception.h"

/* --- Process Table Limits --- */
#define PROCESS_TABLE_SIZE 1024

/* --- User Virtual Address Space --- */
#define USER_VA_BASE             0x40000000ULL /* 1GB - leaves lower area for ELF loading */
#define USER_VA_MAX_REGIONS      16
#define PROCESS_USER_STACK_PAGES 64 /* 64*4 KB user stack */

/* --- Hardware/Architecture Specifics --- */
#define SPSR_EL0_USER      0x00000000ULL
#define SPSR_EL1_KERN      0x000003C5ULL
#define STACK_CANARY_VALUE 0xDEADC0DEDEADC0DEULL

typedef enum
{
    PROCESS_STATE_EMPTY = 0,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_ZOMBIE,
    PROCESS_STATE_DEAD
} process_state_t;

/* Represents a contiguous block of allocated virtual memory */
struct va_region
{
    uintptr_t base;
    size_t pages;
};

/* Tracks allocated virtual memory regions for a specific process */
struct va_allocator
{
    struct va_region regions[USER_VA_MAX_REGIONS];
    size_t count;
    uintptr_t next_va;
};

/* The central Process Control Block (PCB) */
struct process
{
    /* Identifiers and Status */
    uint32_t pid;
    char name[64];
    process_state_t state;
    uint32_t parent_pid;
    struct task* main_task;
    int exit_status;

    /* Physical Memory Layout */
    uintptr_t paddr_code;
    uintptr_t paddr_user_stack;
    uintptr_t paddr_kernel_stack;

    /* Virtual Memory Layout */
    uintptr_t vaddr_code;
    uintptr_t vaddr_user_stack;
    uintptr_t vaddr_kernel_stack;

    /* Paging and MMU context */
    unsigned long* user_pgd; /* per-process TTBR0 page table */
    unsigned long asid;      /* address space ID for TLB tagging */
    unsigned long ttbr0;     /* hardware-ready TTBR0 value (Phys PGD | ASID) */

    /* Execution Context */
    struct cpu_context context;
    struct va_allocator va; /* per-process user VA allocator */

    /* File System Context */
    struct vfs_file* fd_table[VFS_MAX_FDS];
    spinlock_t fd_lock;
    struct vfs_vnode* cwd;

    /* Signals */
    sigset_t pending_signals;
    sigset_t blocked_signals;
    struct sigaction signal_handlers[SIGNAL_COUNT];
    uintptr_t default_sigrestorer;
};

extern struct process process_table[PROCESS_TABLE_SIZE];
extern spinlock_t process_table_lock;

/* --- Initialization & Lifecycle --- */
void process_init(void);
void process_create(void* code_ptr, size_t code_size, uint32_t pid);
int process_create_from_file(const char* path, uint32_t pid);
int process_exec(const char* path, char* const argv[]);
int process_fork(struct exception_trap_frame* parent_tf);
void process_exit(uint32_t pid, int status);
int process_waitpid(int pid, int* status, int options);

/* --- Context Switching & Execution --- */
void process_drop_to_user(void* code_vaddr, void* stack_vaddr);
int process_find_current(void); /* Returns PID of current process, -1 if none */

/* --- Memory Management --- */
unsigned long process_get_ttbr0(uint32_t pid); /* Returns TTBR0 for PID, 0 if invalid */
uintptr_t process_va_alloc(struct va_allocator* va, size_t pages);
void process_va_free(struct va_allocator* va, uintptr_t base);

/* * Note: If process_flush_icache_range is purely an architecture utility,
 * consider moving it to an arch/cache.h header instead of process.h.
 */
void process_flush_icache_range(void* start, size_t size);

#endif /* PERSPICUA_KERNEL_PROCESS_H */
