#ifndef PERSPICUA_MM_ASID_H
#define PERSPICUA_MM_ASID_H

#include "core/lock.h"
#include "types.h"

#define BITMAP_SIZE 4
/* Highest allocatable ASID. Derived so it cannot drift from the bitmap. */
#define MAX_ASID (BITMAP_SIZE * 64 - 1)

/*
 * TTBR0 and the TLBI operands carry the ASID in bits [63:48]. boot.S sets
 * TCR_EL1.AS, selecting 16-bit ASIDs, so every site that builds one of those
 * values must use the same width. Masking narrower somewhere would let two
 * address spaces share an effective ASID and alias each other's TLB entries --
 * which stays invisible while the pool fits in 8 bits, and becomes a
 * cross-process disclosure the moment BITMAP_SIZE grows.
 */
#define ASID_MASK       0xFFFFUL
#define ASID_TTBR_SHIFT 48

static inline unsigned long asid_ttbr_field(unsigned long asid)
{
    return (asid & ASID_MASK) << ASID_TTBR_SHIFT;
}

struct asid_pool_t {
    uint64_t bitmap[BITMAP_SIZE];
    uint64_t generation;
};

void asid_init(void);
void asid_get_active(unsigned long *asid_out, unsigned long *gen_out);
void asid_free(unsigned long *asid, unsigned long *gen);

#endif
