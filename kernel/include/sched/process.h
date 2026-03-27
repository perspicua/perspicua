/*
 * process.h - Public API for process and address space management.
 *
 * This header defines the Process Control Block (PCB), virtual address
 * space tracking, and the lifecycle management interface.
 */

#ifndef PERSPICUA_SCHED_PROCESS_H
#define PERSPICUA_SCHED_PROCESS_H

#include "types.h"

#include "uapi/wait.h"

#include "arch/exception.h"

#include "core/signals.h"
#include "core/lock.h"
#include "fs/vfs.h"
#include "sched/sched.h"

/* --- Constants and Macros --- */

#define PROCESS_TABLE_SIZE       1024
#define USER_VA_BASE             0x40000000ULL
#define USER_VA_MAX_REGIONS      16
#define PROCESS_USER_STACK_PAGES 64

#define SPSR_EL0_USER      0x00000000ULL
#define SPSR_EL1_KERN      0x000003C5ULL
#define STACK_CANARY_VALUE 0xDEADC0DEDEADC0DEULL

/* --- Enums and Types --- */

typedef enum {
    PROCESS_STATE_EMPTY = 0,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_ZOMBIE,
    PROCESS_STATE_DEAD
} process_state_t;

/* --- Data Structures --- */

struct va_region {
    uintptr_t base;
    size_t pages;
};

struct va_allocator {
    struct va_region regions[USER_VA_MAX_REGIONS];
    size_t count;
    uintptr_t next_va;
};

/*
 * struct process - The central Process Control Block (PCB).
 */
struct process {
    uint32_t pid;
    char name[64];
    process_state_t state;
    uint32_t parent_pid;
    struct task *main_task;
    int exit_status;

    uintptr_t paddr_code;
    uintptr_t paddr_user_stack;
    uintptr_t paddr_kernel_stack;

    uintptr_t vaddr_code;
    uintptr_t vaddr_user_stack;
    uintptr_t vaddr_kernel_stack;

    unsigned long *user_pgd;
    unsigned long asid;
    unsigned long ttbr0;

    struct cpu_context context;
    struct va_allocator va;

    struct vfs_file *fd_table[VFS_MAX_FDS];
    spinlock_t fd_lock;
    struct vfs_vnode *cwd;

    sigset_t pending_signals;
    sigset_t blocked_signals;
    struct sigaction signal_handlers[SIGNAL_COUNT];
    uintptr_t default_sigrestorer;
};

/* --- Global Variables --- */

extern struct process process_table[PROCESS_TABLE_SIZE];
extern spinlock_t process_table_lock;

/* --- Function Prototypes --- */

/* Lifecycle and execution */
void process_init(void);
void process_create(void *code_ptr, size_t code_size, uint32_t pid);
int process_create_from_file(const char *path, uint32_t pid);
int process_exec(const char *path, char *const argv[]);
int process_fork(struct exception_trap_frame *parent_tf);
void process_exit(uint32_t pid, int status);
int process_waitpid(int pid, int *status, int options);

/* Context and identity */
void process_drop_to_user(void *code_vaddr, void *stack_vaddr);
int process_find_current(void);
unsigned long process_get_ttbr0(uint32_t pid);

/* Memory management */
uintptr_t process_va_alloc(struct va_allocator *va, size_t pages);
void process_va_free(struct va_allocator *va, uintptr_t base);
void process_flush_icache_range(void *start, size_t size);

#endif /* PERSPICUA_SCHED_PROCESS_H */
