#include "mmu.h"
#include "pmm.h"
#include "lock.h"
#include "addr.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../lib/panic.h"

#define PTE_VALID MMU_PTE_VALID
#define PTE_TABLE MMU_PTE_TABLE
#define PTE_PAGE MMU_PTE_PAGE
#define PTE_BLOCK MMU_PTE_BLOCK
#define PTE_AF MMU_PTE_AF
#define PTE_SH_INNER MMU_PTE_SH_INNER
#define PTE_PXN MMU_PXN
#define PTE_UXN MMU_UXN
#define PTE_AP_RW MMU_AP_RW
#define PTE_AP_RO MMU_AP_RO
#define PTE_ATTR_NORMAL MMU_ATTR_NORMAL
#define PTE_ATTR_DEVICE MMU_ATTR_DEVICE

extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], __data_end[];
extern char __bss_start[], __bss_end[];
extern char __kernel_end[];

static unsigned long kernel_pgd_phys;
static unsigned long* kernel_pgd_virt; // virtual address of the PGD for dynamic mapping
static unsigned long empty_pgd_phys;   // zeroed PGD for kernel-only tasks (TTBR0)
static spinlock_t mmu_lock = SPINLOCK_INIT;

// --- address field extraction for 39-bit VA, 4KB granule ---
// VA layout: [38:30] = L1 index, [29:21] = L2 index, [20:12] = L3 index, [11:0] = page offset
#define L1_INDEX(va) (((va) >> 30) & 0x1FF)
#define L2_INDEX(va) (((va) >> 21) & 0x1FF)
#define L3_INDEX(va) (((va) >> 12) & 0x1FF)
#define PAGE_OFFSET(va) ((va) & 0xFFF)

// PTE address mask: bits [47:12] hold the physical frame address
#define PTE_ADDR_MASK 0x0000FFFFFFFFF000ULL

void mmu_init(void)
{
    unsigned long* pgd = (unsigned long*)pmm_alloc_page();
    unsigned long* pmd_0 = (unsigned long*)pmm_alloc_page();
    unsigned long* pmd_1 = (unsigned long*)pmm_alloc_page();
    unsigned long* pmd_2 = (unsigned long*)pmm_alloc_page();
    unsigned long* pmd_3 = (unsigned long*)pmm_alloc_page();
    unsigned long* pte_kernel = (unsigned long*)pmm_alloc_page();

    for (int i = 0; i < 512; i++)
    {
        pgd[i] = 0;
        pmd_0[i] = 0;
        pmd_1[i] = 0;
        pmd_2[i] = 0;
        pmd_3[i] = 0;
        pte_kernel[i] = 0;
    }

    // link PGD to PMDs (covers [0, 4GB])
    pgd[0] = V2P(pmd_0) | PTE_VALID | PTE_TABLE;
    pgd[1] = V2P(pmd_1) | PTE_VALID | PTE_TABLE;
    pgd[2] = V2P(pmd_2) | PTE_VALID | PTE_TABLE;
    pgd[3] = V2P(pmd_3) | PTE_VALID | PTE_TABLE;

    // map all 4gb
    unsigned long* pmds[] = {pmd_0, pmd_1, pmd_2, pmd_3};
    for (unsigned long p = 0; p < 4; p++)
    {
        for (unsigned long i = 0; i < 512; i++)
        {
            // calculate exact physical addr for this 2mb block
            unsigned long addr = (p * 1024ULL * 1024ULL * 1024ULL) + (i * 2 * 1024 * 1024);

            unsigned long attr = PTE_VALID | PTE_BLOCK | PTE_AF | PTE_PXN | PTE_UXN | PTE_AP_RW;

            if (p == 0)
                attr |= PTE_SH_INNER | PTE_ATTR_NORMAL;
            else
                attr |= PTE_ATTR_DEVICE;
            pmds[p][i] = addr | attr;
        }
    }

    // PMD_0[0] to lvl 3 kernel table
    pmd_0[0] = V2P(pte_kernel) | PTE_VALID | PTE_TABLE;

    // map kernel [0, 2MB] with strict W^X
    for (unsigned long i = 0; i < 512; i++)
    {
        unsigned long addr = i * 4096;
        unsigned long vaddr = P2V(addr);
        unsigned long attr = PTE_VALID | PTE_PAGE | PTE_AF | PTE_SH_INNER | PTE_ATTR_NORMAL;

        if (vaddr >= (unsigned long)__text_start && vaddr < (unsigned long)__text_end)
        {
            attr |= PTE_AP_RO; // Code: Read-Only, Executable
        }
        else if (vaddr >= (unsigned long)__rodata_start && vaddr < (unsigned long)__rodata_end)
        {
            attr |= PTE_AP_RO | PTE_PXN | PTE_UXN; // Constants: Read-Only, No Execute
        }
        else
        {
            attr |= PTE_AP_RW | PTE_PXN | PTE_UXN; // Data/BSS/Stack: Read-Write, No Execute
        }

        pte_kernel[i] = addr | attr;
    }

    // ensure kernel fits within the 4KB-granule W^X region (first 2MB of physical RAM)
    ASSERT(V2P((unsigned long)__kernel_end) <= 2 * 1024 * 1024);

    kernel_pgd_virt = pgd;
    kernel_pgd_phys = V2P(pgd);

    // allocate an empty PGD for kernel-only tasks so TTBR0 faults on any user VA
    unsigned long* empty_pgd = (unsigned long*)pmm_alloc_page();
    memset(empty_pgd, 0, PAGE_SIZE);
    empty_pgd_phys = V2P(empty_pgd);

    asm volatile("msr ttbr1_el1, %0" : : "r"(kernel_pgd_phys));
    asm volatile("msr ttbr0_el1, %0" : : "r"(empty_pgd_phys));

    asm volatile("tlbi vmalle1is");
    asm volatile("dsb ish");
    asm volatile("isb");

    printf("[  MMU ] TTBR1 → 0x%lx, TTBR0 -> empty (trap user access)\n", kernel_pgd_phys);
    printf("[  MMU ] Mapped: 1 GB RAM (2MB blocks) + 3 GB MMIO (device)\n");
    printf("[  MMU ] Kernel [0, 2MB] — 4KB granule, W^X enforced\n");
    printf("[  MMU ]   .text   [0x%lx — 0x%lx] RO+X\n", (unsigned long)__text_start, (unsigned long)__text_end);
    printf("[  MMU ]   .rodata [0x%lx — 0x%lx] RO+NX\n", (unsigned long)__rodata_start, (unsigned long)__rodata_end);
    printf("[  MMU ]   .data   [0x%lx — 0x%lx] RW+NX\n", (unsigned long)__data_start, (unsigned long)__data_end);
    printf("[  MMU ]   .bss    [0x%lx — 0x%lx] RW+NX\n", (unsigned long)__bss_start, (unsigned long)__bss_end);
}

void mmu_secondary_init(void)
{
    asm volatile("msr ttbr1_el1, %0" : : "r"(kernel_pgd_phys));
    asm volatile("msr ttbr0_el1, %0" : : "r"(empty_pgd_phys));

    asm volatile("tlbi vmalle1is");
    asm volatile("dsb ish");
    asm volatile("isb");
}

// allocate a zeroed page table page from PMM
static unsigned long* alloc_table_page(void)
{
    unsigned long* page = (unsigned long*)pmm_alloc_page();
    ASSERT(page != 0);
    memset(page, 0, PAGE_SIZE);
    return page;
}

// TLB invalidation for a single VA
static inline void tlbi_va(unsigned long vaddr)
{
    // tlbi vale1is operates on VA >> 12
    unsigned long va_shifted = vaddr >> 12;
    asm volatile("dsb ish");
    asm volatile("tlbi vale1is, %0" : : "r"(va_shifted));
    asm volatile("dsb ish");
    asm volatile("isb");
}

// split a 2MB block descriptor into 512 individual 4KB L3 page entries.
// returns the new L3 table (virtual). Updates the L2 entry in-place.
static unsigned long* split_block_to_pages(unsigned long* l2_table, unsigned long l2_idx)
{
    unsigned long block_entry = l2_table[l2_idx];
    unsigned long block_phys = block_entry & PTE_ADDR_MASK;
    // extract attribute bits, drop VALID and BLOCK/TABLE bit
    unsigned long block_attr = block_entry & ~PTE_ADDR_MASK & ~PTE_TABLE;

    unsigned long* l3_table = alloc_table_page();

    for (unsigned long i = 0; i < 512; i++)
    {
        unsigned long page_phys = block_phys + i * PAGE_SIZE;
        // PTE_PAGE (bit 1) must be set for L3 descriptors (same bit as PTE_TABLE)
        l3_table[i] = page_phys | PTE_VALID | PTE_PAGE | block_attr;
    }

    // replace the 2MB block with a table pointer
    l2_table[l2_idx] = V2P(l3_table) | PTE_VALID | PTE_TABLE;

    // flush TLB for the entire 2MB range since all 512 entries changed type
    asm volatile("tlbi vmalle1is");
    asm volatile("dsb ish");
    asm volatile("isb");

    return l3_table;
}

void mmu_map_page(unsigned long vaddr, unsigned long paddr, unsigned long flags)
{
    ASSERT((vaddr & 0xFFF) == 0); // page-aligned
    ASSERT((paddr & 0xFFF) == 0);

    unsigned long irqflags = spin_lock_irqsave(&mmu_lock);

    unsigned long l1_idx = L1_INDEX(vaddr);
    unsigned long l2_idx = L2_INDEX(vaddr);
    unsigned long l3_idx = L3_INDEX(vaddr);

    // L1 (PGD) -> L2 (PMD)
    unsigned long l1_entry = kernel_pgd_virt[l1_idx];
    unsigned long* l2_table;
    if (l1_entry & PTE_VALID)
    {
        // existing L2 table
        l2_table = (unsigned long*)P2V(l1_entry & PTE_ADDR_MASK);
    }
    else
    {
        // allocate new L2 table
        l2_table = alloc_table_page();
        kernel_pgd_virt[l1_idx] = V2P(l2_table) | PTE_VALID | PTE_TABLE;
    }

    // L2 (PMD) -> L3 (PTE)
    unsigned long l2_entry = l2_table[l2_idx];
    unsigned long* l3_table;

    if (l2_entry & PTE_VALID)
    {
        if (!(l2_entry & PTE_TABLE))
        {
            // 2MB block - split into 4KB pages so we can map individually
            l3_table = split_block_to_pages(l2_table, l2_idx);
        }
        else
        {
            l3_table = (unsigned long*)P2V(l2_entry & PTE_ADDR_MASK);
        }
    }
    else
    {
        // allocate new L3 table
        l3_table = alloc_table_page();
        l2_table[l2_idx] = V2P(l3_table) | PTE_VALID | PTE_TABLE;
    }

    // L3 (PTE) — write the actual page mapping
    ASSERT(!(l3_table[l3_idx] & PTE_VALID)); // must not already be mapped
    l3_table[l3_idx] = (paddr & PTE_ADDR_MASK) | PTE_VALID | PTE_PAGE | flags;

    // ensure the new PTE is visible
    tlbi_va(vaddr);

    spin_unlock_irqrestore(&mmu_lock, irqflags);
}

void mmu_unmap_page(unsigned long vaddr)
{
    ASSERT((vaddr & 0xFFF) == 0);

    unsigned long irqflags = spin_lock_irqsave(&mmu_lock);

    unsigned long l1_idx = L1_INDEX(vaddr);
    unsigned long l2_idx = L2_INDEX(vaddr);
    unsigned long l3_idx = L3_INDEX(vaddr);

    // walk L1
    unsigned long l1_entry = kernel_pgd_virt[l1_idx];
    ASSERT(l1_entry & PTE_VALID);
    unsigned long* l2_table = (unsigned long*)P2V(l1_entry & PTE_ADDR_MASK);

    // walk L2
    unsigned long l2_entry = l2_table[l2_idx];
    ASSERT(l2_entry & PTE_VALID);
    unsigned long* l3_table;
    if (!(l2_entry & PTE_TABLE))
    {
        // 2MB block — split into 4KB pages so we can unmap one
        l3_table = split_block_to_pages(l2_table, l2_idx);
    }
    else
    {
        l3_table = (unsigned long*)P2V(l2_entry & PTE_ADDR_MASK);
    }

    // clear L3 entry
    ASSERT(l3_table[l3_idx] & PTE_VALID);
    l3_table[l3_idx] = 0;

    tlbi_va(vaddr);

    spin_unlock_irqrestore(&mmu_lock, irqflags);
}

int mmu_query(unsigned long vaddr, unsigned long* out_paddr, unsigned long* out_flags)
{
    unsigned long irqflags = spin_lock_irqsave(&mmu_lock);

    unsigned long l1_idx = L1_INDEX(vaddr);
    unsigned long l2_idx = L2_INDEX(vaddr);
    unsigned long l3_idx = L3_INDEX(vaddr);

    unsigned long l1_entry = kernel_pgd_virt[l1_idx];
    if (!(l1_entry & PTE_VALID))
    {
        spin_unlock_irqrestore(&mmu_lock, irqflags);
        return 0;
    }

    unsigned long* l2_table = (unsigned long*)P2V(l1_entry & PTE_ADDR_MASK);
    unsigned long l2_entry = l2_table[l2_idx];

    if (!(l2_entry & PTE_VALID))
    {
        spin_unlock_irqrestore(&mmu_lock, irqflags);
        return 0;
    }

    // handle 2MB block descriptors (used by mmu_init for RAM/MMIO)
    if (!(l2_entry & PTE_TABLE))
    {
        unsigned long block_phys = l2_entry & PTE_ADDR_MASK;
        unsigned long offset = vaddr & 0x1FFFFF; // low 21 bits
        if (out_paddr)
            *out_paddr = block_phys + offset;
        if (out_flags)
            *out_flags = l2_entry & ~PTE_ADDR_MASK;
        spin_unlock_irqrestore(&mmu_lock, irqflags);
        return 1;
    }

    unsigned long* l3_table = (unsigned long*)P2V(l2_entry & PTE_ADDR_MASK);
    unsigned long l3_entry = l3_table[l3_idx];

    if (!(l3_entry & PTE_VALID))
    {
        spin_unlock_irqrestore(&mmu_lock, irqflags);
        return 0;
    }

    if (out_paddr)
        *out_paddr = (l3_entry & PTE_ADDR_MASK) + PAGE_OFFSET(vaddr);
    if (out_flags)
        *out_flags = l3_entry & ~PTE_ADDR_MASK;

    spin_unlock_irqrestore(&mmu_lock, irqflags);
    return 1;
}

// --- per-process user page tables (TTBR0) ---

unsigned long* mmu_create_user_pgd(void)
{
    unsigned long* pgd = alloc_table_page();
    return pgd;
}

void mmu_destroy_user_pgd(unsigned long* pgd)
{
    for (int i = 0; i < 512; i++)
    {
        if (!(pgd[i] & PTE_VALID))
            continue;
        if (!(pgd[i] & PTE_TABLE))
            continue; // skip block entries (shouldn't exist in user PGD)

        unsigned long* l2 = (unsigned long*)P2V(pgd[i] & PTE_ADDR_MASK);
        for (int j = 0; j < 512; j++)
        {
            if (!(l2[j] & PTE_VALID))
                continue;
            if (l2[j] & PTE_TABLE)
            {
                // L3 table — free the page table page itself
                unsigned long* l3 = (unsigned long*)P2V(l2[j] & PTE_ADDR_MASK);
                pmm_free_page(l3);
            }
            // block entries: nothing to free (caller frees mapped pages)
        }
        pmm_free_page(l2);
    }
    pmm_free_page(pgd);
}

void mmu_user_map_page(unsigned long* pgd, unsigned long vaddr, unsigned long paddr, unsigned long flags)
{
    ASSERT((vaddr & 0xFFF) == 0);
    ASSERT((paddr & 0xFFF) == 0);

    unsigned long irqflags = spin_lock_irqsave(&mmu_lock);

    unsigned long l1_idx = L1_INDEX(vaddr);
    unsigned long l2_idx = L2_INDEX(vaddr);
    unsigned long l3_idx = L3_INDEX(vaddr);

    // L1 -> L2
    unsigned long* l2_table;
    if (pgd[l1_idx] & PTE_VALID)
    {
        l2_table = (unsigned long*)P2V(pgd[l1_idx] & PTE_ADDR_MASK);
    }
    else
    {
        l2_table = alloc_table_page();
        pgd[l1_idx] = V2P(l2_table) | PTE_VALID | PTE_TABLE;
    }

    // L2 -> L3
    unsigned long* l3_table;
    if (l2_table[l2_idx] & PTE_VALID)
    {
        ASSERT(l2_table[l2_idx] & PTE_TABLE); // must be table, not block
        l3_table = (unsigned long*)P2V(l2_table[l2_idx] & PTE_ADDR_MASK);
    }
    else
    {
        l3_table = alloc_table_page();
        l2_table[l2_idx] = V2P(l3_table) | PTE_VALID | PTE_TABLE;
    }

    ASSERT(!(l3_table[l3_idx] & PTE_VALID));
    l3_table[l3_idx] = (paddr & PTE_ADDR_MASK) | PTE_VALID | PTE_PAGE | flags;

    tlbi_va(vaddr);

    spin_unlock_irqrestore(&mmu_lock, irqflags);
}

void mmu_user_unmap_page(unsigned long* pgd, unsigned long vaddr)
{
    ASSERT((vaddr & 0xFFF) == 0);

    unsigned long irqflags = spin_lock_irqsave(&mmu_lock);

    unsigned long l1_idx = L1_INDEX(vaddr);
    unsigned long l2_idx = L2_INDEX(vaddr);
    unsigned long l3_idx = L3_INDEX(vaddr);

    ASSERT(pgd[l1_idx] & PTE_VALID);
    unsigned long* l2 = (unsigned long*)P2V(pgd[l1_idx] & PTE_ADDR_MASK);

    ASSERT(l2[l2_idx] & PTE_VALID);
    ASSERT(l2[l2_idx] & PTE_TABLE);
    unsigned long* l3 = (unsigned long*)P2V(l2[l2_idx] & PTE_ADDR_MASK);

    ASSERT(l3[l3_idx] & PTE_VALID);
    l3[l3_idx] = 0;

    tlbi_va(vaddr);

    spin_unlock_irqrestore(&mmu_lock, irqflags);
}

void mmu_switch_user(unsigned long* pgd, unsigned long asid)
{
    unsigned long ttbr0 = V2P(pgd) | (asid << 48);
    asm volatile("msr ttbr0_el1, %0" : : "r"(ttbr0));
    asm volatile("isb");
}

int mmu_user_query(unsigned long* pgd, unsigned long vaddr, unsigned long* out_paddr, unsigned long* out_flags)
{
    unsigned long irqflags = spin_lock_irqsave(&mmu_lock);

    unsigned long l1_idx = L1_INDEX(vaddr);
    unsigned long l2_idx = L2_INDEX(vaddr);
    unsigned long l3_idx = L3_INDEX(vaddr);

    if (!(pgd[l1_idx] & PTE_VALID))
    {
        spin_unlock_irqrestore(&mmu_lock, irqflags);
        return 0;
    }

    unsigned long* l2 = (unsigned long*)P2V(pgd[l1_idx] & PTE_ADDR_MASK);
    if (!(l2[l2_idx] & PTE_VALID))
    {
        spin_unlock_irqrestore(&mmu_lock, irqflags);
        return 0;
    }

    if (!(l2[l2_idx] & PTE_TABLE))
    {
        // 2MB block
        unsigned long block_phys = l2[l2_idx] & PTE_ADDR_MASK;
        unsigned long offset = vaddr & 0x1FFFFF;
        if (out_paddr)
            *out_paddr = block_phys + offset;
        if (out_flags)
            *out_flags = l2[l2_idx] & ~PTE_ADDR_MASK;
        spin_unlock_irqrestore(&mmu_lock, irqflags);
        return 1;
    }

    unsigned long* l3 = (unsigned long*)P2V(l2[l2_idx] & PTE_ADDR_MASK);
    if (!(l3[l3_idx] & PTE_VALID))
    {
        spin_unlock_irqrestore(&mmu_lock, irqflags);
        return 0;
    }

    if (out_paddr)
        *out_paddr = (l3[l3_idx] & PTE_ADDR_MASK) + PAGE_OFFSET(vaddr);
    if (out_flags)
        *out_flags = l3[l3_idx] & ~PTE_ADDR_MASK;

    spin_unlock_irqrestore(&mmu_lock, irqflags);
    return 1;
}

unsigned long mmu_kernel_ttbr0(void)
{
    return empty_pgd_phys;
}
