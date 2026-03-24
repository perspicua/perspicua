/*
 * mmu.h - AArch64 Memory Management Unit interface.
 *
 * Provides page table attribute constants, permission combinators,
 * and the kernel MMU API.
 */

#ifndef PERSPICUA_MM_MMU_H
#define PERSPICUA_MM_MMU_H

#include "types.h"

/* AArch64 page-table descriptor bits (ARMv8-A Architecture Reference D5) */

/* Validity and descriptor-type bits [1:0] */
#define MMU_PTE_VALID (1ULL << 0) /* Entry is valid */
#define MMU_PTE_TABLE (1ULL << 1) /* L0-L2: points to next-level table */
#define MMU_PTE_PAGE  (1ULL << 1) /* L3: 4 KB page descriptor */
#define MMU_PTE_BLOCK (0ULL << 1) /* L1/L2: 2 MB / 1 GB block */

/* Access and shareability flags */
#define MMU_PTE_AF       (1ULL << 10) /* Access Flag */
#define MMU_PTE_SH_INNER (3ULL << 8)  /* Inner Shareable */
#define MMU_PTE_SH_OUTER (2ULL << 8)  /* Outer Shareable */
#define MMU_PTE_SH_NS    (0ULL << 8)  /* Non-Shareable */

/* Access Permission bits (AP[2:1], bits [7:6]) */
#define MMU_AP_USER (1ULL << 6) /* EL0 (user) access enable */
#define MMU_AP_RO   (1ULL << 7) /* Read-only */
#define MMU_AP_RW   (0ULL << 7) /* Read/write */

/* Execute-Never bits */
#define MMU_PXN (1ULL << 53)        /* Privileged Execute-Never */
#define MMU_UXN (1ULL << 54)        /* Unprivileged (EL0) Execute-Never */
#define MMU_XN  (MMU_PXN | MMU_UXN) /* Both XN bits */

/* Not-Global bit for per-ASID TLB entries */
#define MMU_PTE_NG (1ULL << 11)

/* Software-defined bits (ignored by hardware) */
#define MMU_PTE_COW    (1ULL << 55) /* Copy-on-Write */
#define MMU_PTE_SHARED (1ULL << 56) /* Shared-mapping hint */

/* MAIR_EL1 index bits */
#define MMU_ATTR_NORMAL    (0ULL << 2) /* Index 0: Normal cacheable */
#define MMU_ATTR_DEVICE    (1ULL << 2) /* Index 1: Device nGnRnE */
#define MMU_ATTR_NORMAL_NC (2ULL << 2) /* Index 2: Normal Non-Cacheable */

/* Physical address mask [47:12] */
#define MMU_PTE_ADDR_MASK 0x0000FFFFFFFFF000ULL

/* High-level page-permission combinators */

/* User-space executable: EL0 R-X, EL1 RO */
#define MMU_PAGE_USER_CODE                                                                                    \
    (MMU_PTE_VALID | MMU_PTE_PAGE | MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_USER | MMU_AP_RO \
     | MMU_PXN | MMU_PTE_NG)

/* User-space writable data: EL0 RW, NX */
#define MMU_PAGE_USER_DATA                                                                                    \
    (MMU_PTE_VALID | MMU_PTE_PAGE | MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_USER | MMU_AP_RW \
     | MMU_PXN | MMU_UXN | MMU_PTE_NG)

/* User-space read-only data: EL0 RO, NX */
#define MMU_PAGE_USER_RODATA                                                                                  \
    (MMU_PTE_VALID | MMU_PTE_PAGE | MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_USER | MMU_AP_RO \
     | MMU_PXN | MMU_UXN | MMU_PTE_NG)

/* Kernel code: EL1 R-X, EL0 NX/No access */
#define MMU_FLAGS_KERNEL_RX (MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_RO | MMU_UXN)

/* Kernel read-only data: EL1 RO, NX */
#define MMU_FLAGS_KERNEL_RO (MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_RO | MMU_PXN | MMU_UXN)

/* Kernel read-write data: EL1 RW, NX */
#define MMU_FLAGS_KERNEL_RW (MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_RW | MMU_PXN | MMU_UXN)

/* MMIO: Device memory, NX */
#define MMU_FLAGS_DEVICE_RW (MMU_PTE_AF | MMU_ATTR_DEVICE | MMU_AP_RW | MMU_PXN | MMU_UXN)

/* Framebuffer: Write-combining, NX */
#define MMU_FLAGS_FRAMEBUFFER (MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL_NC | MMU_AP_RW | MMU_PXN | MMU_UXN)

/* Kernel MMU API */

/* Initialise MMU on the primary core */
void mmu_init(void);

/* Load page tables on secondary cores */
void mmu_secondary_init(void);

/* Map a 4 KB page in the kernel address space */
void mmu_map_page(unsigned long vaddr, unsigned long paddr, unsigned long flags);

/* Unmap a 4 KB page from the kernel address space */
void mmu_unmap_page(unsigned long vaddr);

/* Translate a kernel virtual address to physical */
int mmu_query(unsigned long vaddr, unsigned long* out_paddr, unsigned long* out_flags);

/* Allocate a new user page table root */
unsigned long* mmu_create_user_pgd(void);

/* Recursively free a user address space */
void mmu_destroy_user_pgd(unsigned long* pgd);

/* Deep-copy a user address space with CoW semantics */
unsigned long* mmu_copy_user_pgd(unsigned long* parent_pgd);

/* Map a 4 KB page in a user address space */
void mmu_user_map_page(unsigned long* pgd, unsigned long vaddr, unsigned long paddr, unsigned long flags);

/* Unmap a 4 KB page from a user address space */
void mmu_user_unmap_page(unsigned long* pgd, unsigned long vaddr);

/* Translate a user virtual address to physical */
int mmu_user_query(unsigned long* pgd, unsigned long vaddr, unsigned long* out_paddr, unsigned long* out_flags);

/* Load a user PGD into TTBR0_EL1 */
void mmu_switch_user(unsigned long* pgd, unsigned long asid);

/* Resolve a Copy-on-Write fault */
int mmu_handle_cow(unsigned long* pgd, unsigned long vaddr);

/* Return the physical address of the empty user PGD */
unsigned long mmu_kernel_ttbr0(void);

#endif /* PERSPICUA_MM_MMU_H */
