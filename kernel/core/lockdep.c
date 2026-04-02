/*
 * lockdep.c - Kernel Lock Dependency Validator
 *
 * This module tracks spinlock acquisition order and detects
 * circular dependencies (deadlocks) and recursive locks at runtime.
 */

#include "core/lockdep.h"
#include "core/lock.h"
#include "stdio.h"
#include "panic.h"

#define MAX_HELD_LOCKS 16
#define MAX_LOCK_NODES 1024

static spinlock_t lockdep_lock = SPINLOCK_INIT;
static int lockdep_disabled[4] = {0}; // Indexed by core ID

static uintptr_t held_locks[4][MAX_HELD_LOCKS];
static int held_count[4] = {0};

static uintptr_t lock_nodes[MAX_LOCK_NODES];
static int num_lock_nodes = 0;

/* graph_edges[A][B] == 1 means lock A was acquired before lock B globally */
static uint8_t graph_edges[MAX_LOCK_NODES][MAX_LOCK_NODES / 8];

static int get_core_id(void)
{
    unsigned long core_id;
    asm volatile("mrs %0, mpidr_el1" : "=r"(core_id));
    return core_id & 3;
}

static inline int get_edge(int a, int b)
{
    return (graph_edges[a][b / 8] & (1 << (b % 8))) != 0;
}

static inline void set_edge(int a, int b)
{
    graph_edges[a][b / 8] |= (1 << (b % 8));
}

static int get_or_create_node(uintptr_t lock)
{
    for (int i = 0; i < num_lock_nodes; i++) {
        if (lock_nodes[i] == lock) {
            return i;
        }
    }
    if (num_lock_nodes >= MAX_LOCK_NODES) {
        return -1; // Graph full, quietly stop tracking new locks
    }
    int idx = num_lock_nodes++;
    lock_nodes[idx] = lock;
    return idx;
}

/* Raw spinlock implementation to avoid recursion */
static void raw_spin_lock(spinlock_t *lock)
{
    unsigned int tmp;
    unsigned int one = 1;
    asm volatile("   sevl\n"
                 "1: wfe\n"
                 "2: ldaxr   %w0, [%1]\n"
                 "   cbnz    %w0, 1b\n"
                 "   stxr    %w0, %w2, [%1]\n"
                 "   cbnz    %w0, 2b\n"
                 : "=&r"(tmp)
                 : "r"(&lock->locked), "r"(one)
                 : "memory");
}

static void raw_spin_unlock(spinlock_t *lock)
{
    asm volatile("   stlr    %w0, [%1]\n"
                 "   sev\n"
                 :
                 : "r"(0), "r"(&lock->locked)
                 : "memory");
}

void lockdep_init(void)
{
    pr_info("lockdep: initialized kernel lock dependency validator\n");
}

void lockdep_acquire(spinlock_t *lock)
{
    int core = get_core_id();

    if (lockdep_disabled[core]) {
        return;
    }

    lockdep_disabled[core] = 1;
    raw_spin_lock(&lockdep_lock);

    int count = held_count[core];
    if (count >= MAX_HELD_LOCKS) {
        raw_spin_unlock(&lockdep_lock);
        lockdep_disabled[core] = 0;
        PANIC("lockdep: max held locks exceeded");
    }

    uintptr_t new_addr = (uintptr_t)lock;

    /* Check for recursive lock acquisition */
    for (int i = 0; i < count; i++) {
        if (held_locks[core][i] == new_addr) {
            raw_spin_unlock(&lockdep_lock);
            lockdep_disabled[core] = 0;
            PANIC("lockdep: recursive lock acquisition detected");
        }
    }

    /* Update dependency graph */
    int new_node = get_or_create_node(new_addr);
    if (new_node >= 0) {
        for (int i = 0; i < count; i++) {
            int held_node = get_or_create_node(held_locks[core][i]);
            if (held_node < 0)
                continue;

            if (get_edge(new_node, held_node)) {
                raw_spin_unlock(&lockdep_lock);
                lockdep_disabled[core] = 0;
                pr_err("\n========================================\n");
                pr_err("LOCKDEP: CIRCULAR DEPENDENCY DETECTED!\n");
                pr_err("Core %d is holding lock at %p\n", core, (void *)held_locks[core][i]);
                pr_err("And is trying to acquire lock at %p\n", (void *)new_addr);
                pr_err("But this creates a cycle in the globally observed order.\n");
                pr_err("========================================\n");
                PANIC("lockdep: deadlock cycle");
            }

            if (!get_edge(held_node, new_node)) {
                set_edge(held_node, new_node);

                /* Update transitive closure */
                for (int x = 0; x < num_lock_nodes; x++) {
                    if (get_edge(x, held_node)) {
                        set_edge(x, new_node);
                        for (int y = 0; y < num_lock_nodes; y++) {
                            if (get_edge(new_node, y)) {
                                set_edge(x, y);
                            }
                        }
                    }
                }
                for (int y = 0; y < num_lock_nodes; y++) {
                    if (get_edge(new_node, y)) {
                        set_edge(held_node, y);
                    }
                }
            }
        }
    }

    /* Record the acquisition */
    held_locks[core][count] = new_addr;
    held_count[core] = count + 1;

    raw_spin_unlock(&lockdep_lock);
    lockdep_disabled[core] = 0;
}

void lockdep_release(spinlock_t *lock)
{
    int core = get_core_id();

    if (lockdep_disabled[core]) {
        return;
    }

    lockdep_disabled[core] = 1;
    raw_spin_lock(&lockdep_lock);

    int count = held_count[core];
    uintptr_t addr = (uintptr_t)lock;

    /* Find and remove the lock from the held list (usually the last one) */
    int found = -1;
    for (int i = count - 1; i >= 0; i--) {
        if (held_locks[core][i] == addr) {
            found = i;
            break;
        }
    }

    if (found >= 0) {
        /* Shift remaining locks down */
        for (int i = found; i < count - 1; i++) {
            held_locks[core][i] = held_locks[core][i + 1];
        }
        held_count[core] = count - 1;
    } else {
        /* Releasing a lock we don't hold! */
        raw_spin_unlock(&lockdep_lock);
        lockdep_disabled[core] = 0;
        pr_err("lockdep: attempting to release unheld lock at %p\n", (void *)addr);
        // We don't panic here because it might just be an unbalanced lock,
        // NOTE: but it is definitely a bug worth logging.
    }

    raw_spin_unlock(&lockdep_lock);
    lockdep_disabled[core] = 0;
}
