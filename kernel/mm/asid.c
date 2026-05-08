#include "mm/asid.h"
#include "core/lock.h"

static struct asid_pool_t asid_pool;
static spinlock_t asid_lock = SPINLOCK_INIT;

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

        // Flush TLB
        asm volatile("tlbi vmalle1is\n"
                     "dsb ish\n"
                     "isb");

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
