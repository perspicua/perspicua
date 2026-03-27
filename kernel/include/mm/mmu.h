/*
 * mmu.h - AArch64 Memory Management Unit interface.
 *
 * This header provides constants for page table descriptors, permission
 * combinators, and the kernel MMU management API.
 */

#ifndef PERSPICUA_MM_MMU_H
#define PERSPICUA_MM_MMU_H

#include "types.h"

/* --- Descriptor Bits (ARMv8-A Reference D5) --- */

#define MMU_PTE_VALID (1ULL << 0)
#define MMU_PTE_TABLE (1ULL << 1) /* L0-L2 only */
#define MMU_PTE_PAGE  (1ULL << 1) /* L3 only */
#define MMU_PTE_BLOCK (0ULL << 1) /* L1/L2 only */

#define MMU_PTE_AF       (1ULL << 10)
#define MMU_PTE_SH_INNER (3ULL << 8)
#define MMU_PTE_SH_OUTER (2ULL << 8)
#define MMU_PTE_SH_NS    (0ULL << 8)

#define MMU_AP_USER (1ULL << 6)
#define MMU_AP_RO   (1ULL << 7)
#define MMU_AP_RW   (0ULL << 7)

#define MMU_PXN (1ULL << 53)
#define MMU_UXN (1ULL << 54)
#define MMU_XN  (MMU_PXN | MMU_UXN)

#define MMU_PTE_NG (1ULL << 11)

/* Software-defined bits */
#define MMU_PTE_COW    (1ULL << 55)
#define MMU_PTE_SHARED (1ULL << 56)

/* MAIR_EL1 index bits */
#define MMU_ATTR_NORMAL    (0ULL << 2)
#define MMU_ATTR_DEVICE    (1ULL << 2)
#define MMU_ATTR_NORMAL_NC (2ULL << 2)

#define MMU_PTE_ADDR_MASK 0x0000FFFFFFFFF000ULL

/* --- High-level Permission Combinators --- */

#define MMU_PAGE_USER_CODE                                                                        \
    (MMU_PTE_VALID | MMU_PTE_PAGE | MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_USER \
     | MMU_AP_RO | MMU_PXN | MMU_PTE_NG)

#define MMU_PAGE_USER_DATA                                                                        \
    (MMU_PTE_VALID | MMU_PTE_PAGE | MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_USER \
     | MMU_AP_RW | MMU_PXN | MMU_UXN | MMU_PTE_NG)

#define MMU_PAGE_USER_RODATA                                                                      \
    (MMU_PTE_VALID | MMU_PTE_PAGE | MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_USER \
     | MMU_AP_RO | MMU_PXN | MMU_UXN | MMU_PTE_NG)

#define MMU_FLAGS_KERNEL_RX (MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_RO | MMU_UXN)

#define MMU_FLAGS_KERNEL_RO \
    (MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_RO | MMU_PXN | MMU_UXN)

#define MMU_FLAGS_KERNEL_RW \
    (MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_RW | MMU_PXN | MMU_UXN)

#define MMU_FLAGS_DEVICE_RW (MMU_PTE_AF | MMU_ATTR_DEVICE | MMU_AP_RW | MMU_PXN | MMU_UXN)

#define MMU_FLAGS_FRAMEBUFFER \
    (MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL_NC | MMU_AP_RW | MMU_PXN | MMU_UXN)

/* --- Function Prototypes --- */

void mmu_init(void);
void mmu_secondary_init(void);

void mmu_map_page(unsigned long vaddr, unsigned long paddr, unsigned long flags);
void mmu_unmap_page(unsigned long vaddr);
int mmu_query(unsigned long vaddr, unsigned long *out_paddr, unsigned long *out_flags);

unsigned long *mmu_create_user_pgd(void);
void mmu_destroy_user_pgd(unsigned long *pgd);
unsigned long *mmu_copy_user_pgd(unsigned long *parent_pgd);

void mmu_user_map_page(unsigned long *pgd, unsigned long vaddr, unsigned long paddr,
                       unsigned long flags);
void mmu_user_unmap_page(unsigned long *pgd, unsigned long vaddr);
int mmu_user_query(unsigned long *pgd, unsigned long vaddr, unsigned long *out_paddr,
                   unsigned long *out_flags);

void mmu_switch_user(unsigned long *pgd, unsigned long asid);
int mmu_handle_cow(unsigned long *pgd, unsigned long vaddr);

unsigned long mmu_kernel_ttbr0(void);

#endif /* PERSPICUA_MM_MMU_H */
