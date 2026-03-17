/*
 * mmu.h - Public API for the Memory Management Unit (MMU) driver.
 *
 * This file defines the Page Table Entry (PTE) bits, memory attributes,
 * and functions for managing kernel and user page tables on AArch64.
 */

#ifndef PERSPICUA_KERNEL_MMU_H
#define PERSPICUA_KERNEL_MMU_H

#include "types.h"

/* Page Table Entry (PTE) attribute bits */
#define MMU_PTE_VALID    (1ULL << 0)
#define MMU_PTE_TABLE    (1ULL << 1)  /* L1/L2: points to next-level table */
#define MMU_PTE_PAGE     (1ULL << 1)  /* L3: 4KB page descriptor */
#define MMU_PTE_BLOCK    (0ULL << 1)  /* L1/L2: 2MB/1GB block descriptor */
#define MMU_PTE_AF       (1ULL << 10) /* Access flag (must be set for entry to be valid) */
#define MMU_PTE_SH_INNER (3ULL << 8)  /* Inner shareable memory */

/* Access Permissions (AP) */
#define MMU_AP_RW   (0ULL << 6) /* Read/Write */
#define MMU_AP_RO   (2ULL << 6) /* Read-Only */
#define MMU_AP_USER (1ULL << 6) /* User-mode access bit */

/* Execute-Never (XN) bits */
#define MMU_PXN (1ULL << 53) /* Privileged Execute-Never */
#define MMU_UXN (1ULL << 54) /* Unprivileged Execute-Never */

/* Software-defined PTE bits */
#define MMU_PTE_COW (1ULL << 55) /* Copy-on-Write flag */

/* Memory attribute indices (must match MAIR_EL1 configuration) */
#define MMU_ATTR_NORMAL (0ULL << 2) /* MAIR index 0: Normal cacheable */
#define MMU_ATTR_DEVICE (1ULL << 2) /* MAIR index 1: Device nGnRE */

/* High-level page permission combinations for user processes */
#define MMU_PAGE_USER_CODE \
    (MMU_PTE_VALID | MMU_PTE_PAGE | MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_USER | MMU_PXN)

#define MMU_PAGE_USER_DATA \
    (MMU_PTE_VALID | MMU_PTE_PAGE | MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_USER | MMU_PXN | MMU_UXN)

/* Standard kernel memory mapping flags */
#define MMU_FLAGS_KERNEL_RW (MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_RW | MMU_PXN | MMU_UXN)
#define MMU_FLAGS_KERNEL_RO (MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_RO | MMU_PXN | MMU_UXN)
#define MMU_FLAGS_KERNEL_RX (MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_RO)
#define MMU_FLAGS_DEVICE_RW (MMU_PTE_AF | MMU_ATTR_DEVICE | MMU_AP_RW | MMU_PXN | MMU_UXN)

/*
 * mmu_init - Boot-time initialization of the kernel's virtual memory system.
 * Sets up identity mapping for the first 4GB and establishes the higher-half kernel.
 */
void mmu_init(void);

/*
 * mmu_secondary_init - Secondary CPU core initialization for the MMU.
 * Loads the kernel PGD and empty user PGD into TTBR1 and TTBR0.
 */
void mmu_secondary_init(void);

/*
 * mmu_map_page - Establishes a 4KB mapping in the kernel's virtual address space.
 * Intermediate page tables are allocated from the PMM as needed.
 */
void mmu_map_page(unsigned long vaddr, unsigned long paddr, unsigned long flags);

/*
 * mmu_unmap_page - Removes a 4KB mapping from the kernel's virtual address space.
 */
void mmu_unmap_page(unsigned long vaddr);

/*
 * mmu_query - Checks if a kernel virtual address is mapped. Returns non-zero
 * if valid, and optionally outputs the physical address and descriptor flags.
 */
int mmu_query(unsigned long vaddr, unsigned long* out_paddr, unsigned long* out_flags);

/*
 * mmu_create_user_pgd - Allocates and initializes a fresh Page Global Directory
 * for a new user process.
 */
unsigned long* mmu_create_user_pgd(void);

/*
 * mmu_copy_user_pgd - Duplicates an existing user address space for process
 * forking, establishing Copy-on-Write (CoW) protections where appropriate.
 */
unsigned long* mmu_copy_user_pgd(unsigned long* parent_pgd);

/*
 * mmu_destroy_user_pgd - Recursively frees all page tables associated with
 * a user process address space.
 */
void mmu_destroy_user_pgd(unsigned long* pgd);

/*
 * mmu_user_map_page - Establishes a 4KB mapping in a user process address space.
 */
void mmu_user_map_page(unsigned long* pgd, unsigned long vaddr, unsigned long paddr, unsigned long flags);

/*
 * mmu_user_unmap_page - Removes a 4KB mapping from a user process address space.
 */
void mmu_user_unmap_page(unsigned long* pgd, unsigned long vaddr);

/*
 * mmu_user_query - Checks if a user virtual address is mapped in the given PGD.
 */
int mmu_user_query(unsigned long* pgd, unsigned long vaddr, unsigned long* out_paddr, unsigned long* out_flags);

/*
 * mmu_handle_cow - Resolves a Copy-on-Write fault by allocating a new page,
 * copying the original data, and updating the page table with write permissions.
 */
int mmu_handle_cow(unsigned long* pgd, unsigned long vaddr);

/*
 * mmu_switch_user - Activates a user address space by loading its PGD into TTBR0.
 */
void mmu_switch_user(unsigned long* pgd, unsigned long asid);

/*
 * mmu_kernel_ttbr0 - Returns the physical address of the empty kernel PGD
 * used for kernel-only tasks.
 */
unsigned long mmu_kernel_ttbr0(void);

#endif /* PERSPICUA_KERNEL_MMU_H */
