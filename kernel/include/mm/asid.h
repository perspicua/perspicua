/*
 * asid.h - Address-space IDs: the TTBR0 field and the per-process allocator.
 */

#ifndef PERSPICUA_MM_ASID_H
#define PERSPICUA_MM_ASID_H

// TCR_EL1.AS selects 16-bit ASIDs, so every TTBR0 and TLBI operand masks to this one width.
#define ASID_MASK       0xFFFFUL
#define ASID_TTBR_SHIFT 48

static inline unsigned long asid_ttbr_field(unsigned long asid)
{
    return (asid & ASID_MASK) << ASID_TTBR_SHIFT;
}

void asid_init(void);
void asid_get_active(unsigned long *asid_out, unsigned long *gen_out);
void asid_free(unsigned long *asid, unsigned long *gen);

#endif // PERSPICUA_MM_ASID_H
