#ifndef _MMU_H_
#define _MMU_H_

#include "../lib/types.h"

#define MMU_PTE_VALID (1ULL << 0)
#define MMU_PTE_TABLE (1ULL << 1)    // L1/L2: points to next-level table
#define MMU_PTE_PAGE (1ULL << 1)     // L3:    4KB page descriptor
#define MMU_PTE_BLOCK (0ULL << 1)    // L1/L2: 2MB/1GB block descriptor
#define MMU_PTE_AF (1ULL << 10)      // access flag (must be set)
#define MMU_PTE_SH_INNER (3ULL << 8) // inner shareable

// access permissions
#define MMU_AP_RW (0ULL << 6)
#define MMU_AP_RO (2ULL << 6)

#define MMU_AP_USER (1ULL << 6)
#define PAGE_USER_CODE \
    (MMU_PTE_VALID | MMU_PTE_PAGE | MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_USER | MMU_PXN)
#define PAGE_USER_DATA \
    (MMU_PTE_VALID | MMU_PTE_PAGE | MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_USER | MMU_PXN | MMU_UXN)
// execute-never bits
#define MMU_PXN (1ULL << 53) // privileged execute-never
#define MMU_UXN (1ULL << 54) // unprivileged execute-never

// memory attribute indices (must match MAIR_EL1 setup)
#define MMU_ATTR_NORMAL (0ULL << 2) // MAIR index 0: normal cacheable
#define MMU_ATTR_DEVICE (1ULL << 2) // MAIR index 1: device nGnRE

// convenience flag combinations
#define MMU_FLAGS_KERNEL_RW (MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_RW | MMU_PXN | MMU_UXN)
#define MMU_FLAGS_KERNEL_RO (MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_RO | MMU_PXN | MMU_UXN)
#define MMU_FLAGS_KERNEL_RX (MMU_PTE_AF | MMU_PTE_SH_INNER | MMU_ATTR_NORMAL | MMU_AP_RO)
#define MMU_FLAGS_DEVICE_RW (MMU_PTE_AF | MMU_ATTR_DEVICE | MMU_AP_RW | MMU_PXN | MMU_UXN)

// --- init (called at boot) ---
void mmu_init(void);
void mmu_secondary_init(void);

// --- dynamic page table management (kernel TTBR1) ---

// Map a single 4KB page: vaddr -> paddr with given flags.
// Allocates intermediate page tables (L1/L2) if needed via PMM.
// Panics if the page is already mapped (use mmu_unmap_page first).
void mmu_map_page(unsigned long vaddr, unsigned long paddr, unsigned long flags);

// Unmap a single 4KB page. TLB is invalidated.
// Panics if the page is not currently mapped.
void mmu_unmap_page(unsigned long vaddr);

// Query whether a vaddr is mapped. Returns 1 if mapped, 0 if not.
// If mapped and out_paddr is non-NULL, writes the physical address.
// If mapped and out_flags is non-NULL, writes the PTE flags.
int mmu_query(unsigned long vaddr, unsigned long* out_paddr, unsigned long* out_flags);

// --- per-process user page tables (TTBR0) ---

// Allocate a fresh zeroed PGD for a user process.
// Returns the virtual address of the PGD page.
unsigned long* mmu_create_user_pgd(void);

// Free a user PGD and all L2/L3 tables it references.
// Does NOT free the physical pages that were mapped (caller handles that).
void mmu_destroy_user_pgd(unsigned long* pgd);

// Map a 4KB page in a user PGD (TTBR0 address space).
void mmu_user_map_page(unsigned long* pgd, unsigned long vaddr, unsigned long paddr, unsigned long flags);

// Unmap a 4KB page in a user PGD.
void mmu_user_unmap_page(unsigned long* pgd, unsigned long vaddr);

// Query a user PGD. Returns 1 if vaddr is mapped, 0 if not.
int mmu_user_query(unsigned long* pgd, unsigned long vaddr, unsigned long* out_paddr, unsigned long* out_flags);

// Load a user PGD into TTBR0 with the given ASID. Flushes TLB.
void mmu_switch_user(unsigned long* pgd, unsigned long asid);

// Returns the physical TTBR0 value for an empty (kernel-only) address space.
unsigned long mmu_kernel_ttbr0(void);

#endif // _MMU_H_
