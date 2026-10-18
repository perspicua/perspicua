/*
 * process.h - Public API for process and address space management.
 */

#ifndef PERSPICUA_SCHED_PROCESS_H
#define PERSPICUA_SCHED_PROCESS_H

#include <stddef.h>
#include <stdint.h>

#include "uapi/wait.h"

#include "arch/exception.h"

#include "core/lock.h"
#include "core/signals.h"
#include "fs/vfs.h"
#include "sched/sched.h"

// Kept at 128: at 256, stress.c's fork storm gave children corrupted stacks, still unexplained.
#ifdef CONFIG_MAX_PROCESSES
    #define PROCESS_TABLE_SIZE CONFIG_MAX_PROCESSES
#else
    #define PROCESS_TABLE_SIZE 128
#endif

// The allocator's range, above where programs link, so elf_load's segments never collide with it.
#define USER_VA_BASE        0x40000000ULL
#define USER_VA_LIMIT       0x8000000000ULL
#define USER_VA_MAX_REGIONS 64

// Allocated and mapped up front, touched or not, so this is real memory per process.
#define PROCESS_USER_STACK_PAGES 32

#define SPSR_EL0_USER 0x00000000ULL

// From 1, so a zeroed PCB reads as invalid; a free slot is a NULL entry, never a state.
typedef enum {
    PROCESS_STATE_RUNNING = 1,
    PROCESS_STATE_ZOMBIE,
    PROCESS_STATE_DEAD
} process_state_t;

struct va_region {
    uintptr_t base;
    size_t pages;
};

// regions[] is sorted by base, so one pass finds the lowest gap that fits.
struct va_allocator {
    struct va_region regions[USER_VA_MAX_REGIONS];
    size_t count;
};

struct process {
    uint32_t pid;
    uint32_t pgid;
    uint32_t sid;
    int has_execed;
    int stop_reported;
    int stop_sig; // the signal behind the current stop, as waitpid reports it
    char name[64];
    process_state_t state;
    uint32_t parent_pid;
    struct task *main_task;
    int exit_status;
    uint64_t cpu_ticks;   // the task's CPU time, kept once main_task is gone
    uint64_t start_ticks; // when the slot was claimed: with the pid, it names one process

    uintptr_t paddr_code;
    uintptr_t paddr_user_stack;
    uintptr_t paddr_kernel_stack;

    uintptr_t vaddr_code;
    uintptr_t vaddr_user_stack;
    uintptr_t vaddr_kernel_stack; // kstack_alloc base; see kstack_top()

    unsigned long *user_pgd;
    unsigned long asid;
    unsigned long asid_generation;
    unsigned long ttbr0;

    struct va_allocator va;

    struct vfs_file *fd_table[VFS_MAX_FDS];
    int fd_flags[VFS_MAX_FDS];
    spinlock_t fd_lock;
    struct vfs_vnode *cwd;

    sigset_t pending_signals;
    sigset_t blocked_signals;

    // The mask sigsuspend displaced, parked until a handler frame carries it.
    sigset_t saved_sigmask;
    int has_saved_sigmask;

    struct sigaction signal_handlers[NSIG];
    stack_t sigaltstack;
};

// A NULL entry is a free slot; entries are published and cleared under process_table_lock.
extern struct process *process_table[PROCESS_TABLE_SIZE];
extern spinlock_t process_table_lock;

// Successful forks since boot, as /proc/stat's "processes" reports them.
extern unsigned long process_forks;

// NULL for a free slot or a pid out of range.
static inline struct process *process_slot(uint32_t pid)
{
    return pid < PROCESS_TABLE_SIZE ? process_table[pid] : NULL;
}

// Lifecycle and execution
void process_init(void);
int process_create_from_file(const char *path, uint32_t pid);
int process_exec(const char *path, char *const argv[], char *const envp[]);
int process_fork(struct exception_trap_frame *parent_tf);
// The status a process killed by sig reports, as sh would show it.
#define PROCESS_KILLED_BY(sig) (128 + (sig))

__attribute__((noreturn)) void process_exit(uint32_t pid, int status);
int process_waitpid(int pid, int *status, int options);

// Context and identity
int process_current_pid(void);

// Resolves the pid and its slot together, so a caller never holds a pid whose slot is gone.
struct process *process_current(void);

unsigned long process_get_ttbr0(uint32_t pid);

#ifdef CONFIG_TESTS
// Reserves a free PID slot, zeroed and RUNNING. Returns the pid or negative.
int process_test_claim_slot(void);

// Frees a slot claimed by the above.
void process_test_release_slot(uint32_t pid);
#endif

// Memory management
uintptr_t process_va_alloc(struct va_allocator *va, size_t pages);
void process_va_free(struct va_allocator *va, uintptr_t base);
void process_flush_icache_range(void *start, size_t size);

#endif // PERSPICUA_SCHED_PROCESS_H
