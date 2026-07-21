#include "mm/asid.h"
#include "core/lock.h"

#define ASID_MAX_CORES 4

static struct asid_pool_t asid_pool;
static spinlock_t asid_lock = SPINLOCK_INIT;

/*
 * Last ASID generation each core has synchronized with. A core whose value is
 * behind asid_pool.generation may hold TLB entries for ASIDs that have since
 * been recycled into a new generation, so it must flush its local TLB once
 * before using any ASID of the current generation. This closes the aliasing
 * window that a broadcast rollover flush alone leaves open for a process that
 * keeps running (and refilling its TLB) on another core across the rollover.
 */
static unsigned long cpu_asid_gen[ASID_MAX_CORES];

static inline int asid_core_id(void)
{
    unsigned long mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (int)(mpidr & (ASID_MAX_CORES - 1));
}

void asid_init(void)
{
    spin_lock(&asid_lock);
    for (int i = 0; i < BITMAP_SIZE; i++) {
        asid_pool.bitmap[i] = 0;
    }
    asid_pool.generation = 1;

    // ASID 0 reserved for kernel mappings
    asid_pool.bitmap[0] |= 1ULL;
    spin_unlock(&asid_lock);
}

static int get_free_asid(void)
{
    for (int i = 0; i < BITMAP_SIZE; i++) {
        // If the bitmap is not full, find the first free ASID
        if (asid_pool.bitmap[i] != ~0ULL) {
            for (int bit = 0; bit < 64; bit++) {
                if ((asid_pool.bitmap[i] & (1ULL << bit)) == 0) {
                    asid_pool.bitmap[i] |= (1ULL << bit);
                    return i * 64 + bit;
                }
            }
        }
    }
    return -1; // Pool is full
}

void asid_get_active(unsigned long *asid_out, unsigned long *gen_out)
{
    spin_lock(&asid_lock);

    /* If this core has not yet caught up to the current generation, its local
     * TLB may hold stale entries for recycled ASIDs. Flush once (local only)
     * before using any ASID of this generation. */
    int cpu = asid_core_id();
    if (cpu_asid_gen[cpu] != asid_pool.generation) {
        asm volatile("tlbi vmalle1\n"
                     "dsb nsh\n"
                     "isb");
        cpu_asid_gen[cpu] = asid_pool.generation;
    }

    if (*gen_out == asid_pool.generation && *asid_out != 0) {
        spin_unlock(&asid_lock);
        return; // Current ASID is still valid
    }

    int new_asid = get_free_asid();

    if (new_asid == -1) {
        asid_pool.generation++;

        // Clear the bitmap for the new generation except for ASID 0
        asid_pool.bitmap[0] &= 1ULL;
        for (int i = 1; i < BITMAP_SIZE; i++) {
            asid_pool.bitmap[i] = 0;
        }

        // Flush all cores' TLBs (broadcast) for the rollover.
        asm volatile("tlbi vmalle1is\n"
                     "dsb ish\n"
                     "isb");

        /* This core just flushed and is switching to the newly allocated ASID,
         * so it is caught up. Other cores stay behind and will flush locally on
         * their next asid_get_active(). */
        cpu_asid_gen[cpu] = asid_pool.generation;

        new_asid = get_free_asid();
    }
    *asid_out = (unsigned long)new_asid;
    *gen_out = asid_pool.generation;

    spin_unlock(&asid_lock);
}

void asid_free(unsigned long *asid, unsigned long *gen)
{
    unsigned long flags = spin_lock_irqsave(&asid_lock);

    if (*gen == asid_pool.generation && *asid != 0) {
        unsigned long asid_field = (*asid & 0xFFUL) << 48;
        asm volatile("dsb ish\n"
                     "tlbi aside1is, %0\n"
                     "dsb ish\n"
                     "isb"
                     :
                     : "r"(asid_field)
                     : "memory");

        int index = *asid / 64;
        int bit = *asid % 64;
        asid_pool.bitmap[index] &= ~(1ULL << bit);
    }
    spin_unlock_irqrestore(&asid_lock, flags);

    *asid = 0;
    *gen = 0;
}
