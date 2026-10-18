/*
 * asid.c - The pool of 16-bit address-space IDs, recycled a generation at a time.
 */

#include "mm/asid.h"

#include <stdint.h>

#include "core/lock.h"

#define ASID_BITMAP_WORDS 4

struct asid_pool {
    uint64_t bitmap[ASID_BITMAP_WORDS];
    uint64_t generation;
};

static struct asid_pool asid_pool;
static spinlock_t asid_lock = SPINLOCK_INIT;

// The generation each core last flushed at; a core behind the pool's may cache recycled ASIDs.
static unsigned long cpu_asid_gen[CPU_MAX_CORES];

void asid_init(void)
{
    // Boot-time and single-threaded; no interrupt can contend for this yet.
    spin_lock(&asid_lock);
    for (int i = 0; i < ASID_BITMAP_WORDS; i++) {
        asid_pool.bitmap[i] = 0;
    }
    asid_pool.generation = 1;

    // ASID 0 reserved for kernel mappings
    asid_pool.bitmap[0] |= 1ULL;
    spin_unlock(&asid_lock);
}

static int get_free_asid(void)
{
    for (int i = 0; i < ASID_BITMAP_WORDS; i++) {
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
    unsigned long flags = spin_lock_irqsave(&asid_lock);

    // Behind the pool's generation, this core's TLB may cache recycled ASIDs: flush it once.
    int cpu = cpu_id();
    if (cpu_asid_gen[cpu] != asid_pool.generation) {
        asm volatile("tlbi vmalle1\n"
                     "dsb nsh\n"
                     "isb");
        cpu_asid_gen[cpu] = asid_pool.generation;
    }

    if (*gen_out == asid_pool.generation && *asid_out != 0) {
        spin_unlock_irqrestore(&asid_lock, flags);
        return; // Current ASID is still valid
    }

    int new_asid = get_free_asid();

    if (new_asid == -1) {
        asid_pool.generation++;

        asid_pool.bitmap[0] &= 1ULL;
        for (int i = 1; i < ASID_BITMAP_WORDS; i++) {
            asid_pool.bitmap[i] = 0;
        }

        asm volatile("tlbi vmalle1is\n"
                     "dsb ish\n"
                     "isb");

        // This core just flushed; the others flush on their next asid_get_active().
        cpu_asid_gen[cpu] = asid_pool.generation;

        new_asid = get_free_asid();
    }
    *asid_out = (unsigned long)new_asid;
    *gen_out = asid_pool.generation;

    spin_unlock_irqrestore(&asid_lock, flags);
}

void asid_free(unsigned long *asid, unsigned long *gen)
{
    unsigned long flags = spin_lock_irqsave(&asid_lock);

    if (*gen == asid_pool.generation && *asid != 0) {
        unsigned long asid_field = asid_ttbr_field(*asid);
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
