/*
 * mmu.c - Implementation of the Memory Management Unit (MMU) driver.
 *
 * This file handles the management of AArch64 page tables, including
 * identity mapping, higher-half kernel established, and per-process
 * address space isolation with support for Copy-on-Write.
 */

#include "mm/mmu.h"

#include "mm/pmm.h"
#include "mm/addr.h"
#include "core/lock.h"
#include "stdio.h"
#include "string.h"
#include "panic.h"

/* Internal PTE short-hand macros for clarity */
#define PTE_VALID       MMU_PTE_VALID
#define PTE_TABLE       MMU_PTE_TABLE
#define PTE_PAGE        MMU_PTE_PAGE
#define PTE_BLOCK       MMU_PTE_BLOCK
#define PTE_AF          MMU_PTE_AF
#define PTE_SH_INNER    MMU_PTE_SH_INNER
#define PTE_PXN         MMU_PXN
#define PTE_UXN         MMU_UXN
#define PTE_AP_RW       MMU_AP_RW
#define PTE_AP_RO       MMU_AP_RO
#define PTE_ATTR_NORMAL MMU_ATTR_NORMAL
#define PTE_ATTR_DEVICE MMU_ATTR_DEVICE

/* Extern symbols from the linker script and PMM metadata */
extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], __data_end[];
extern char __bss_start[], __bss_end[];
extern char __kernel_end[];
extern unsigned long pmm_metadata_end;

/* Global MMU state and synchronization */
static unsigned long kernel_pgd_phys;
static unsigned long* kernel_pgd_virt;
static unsigned long empty_pgd_phys;
static spinlock_t mmu_lock = SPINLOCK_INIT;

/* Address field extraction for 39-bit virtual addresses, 4KB granule */
#define L1_INDEX(va)    (((va) >> 30) & 0x1FF)
#define L2_INDEX(va)    (((va) >> 21) & 0x1FF)
#define L3_INDEX(va)    (((va) >> 12) & 0x1FF)
#define PAGE_OFFSET(va) ((va) & 0xFFF)

/* PTE address mask: bits [47:12] hold the physical frame address */
#define PTE_ADDR_MASK 0x0000FFFFFFFFF000ULL

/*
 * tlbi_va - Invalidates the TLB entry for a specific virtual address.
 */
static inline void tlbi_va(unsigned long vaddr)
{
    unsigned long va_shifted = vaddr >> 12;
    asm volatile("dsb ish");
    asm volatile("tlbi vale1is, %0" : : "r"(va_shifted));
    asm volatile("dsb ish");
    asm volatile("isb");
}

/*
 * alloc_table_page - Internal helper to allocate a zeroed page table.
 */
static unsigned long* alloc_table_page(void)
{
    unsigned long* page = (unsigned long*)pmm_alloc_page();
    if (!page)
    {
        PANIC("MMU: Failed to allocate table page.");
    }
    memset(page, 0, PAGE_SIZE);
    return page;
}

/*
 * mmu_init - Boot-time initialization of the kernel address space.
 */
void mmu_init(void)
{
    unsigned long* pgd = (unsigned long*)pmm_alloc_page();
    unsigned long* pmd_0 = (unsigned long*)pmm_alloc_page();
    unsigned long* pmd_1 = (unsigned long*)pmm_alloc_page();
    unsigned long* pmd_2 = (unsigned long*)pmm_alloc_page();
    unsigned long* pmd_3 = (unsigned long*)pmm_alloc_page();

    memset(pgd, 0, PAGE_SIZE);
    memset(pmd_0, 0, PAGE_SIZE);
    memset(pmd_1, 0, PAGE_SIZE);
    memset(pmd_2, 0, PAGE_SIZE);
    memset(pmd_3, 0, PAGE_SIZE);

    // Link the top-level PGD to PMDs covering the first 4GB
    pgd[0] = V2P(pmd_0) | PTE_VALID | PTE_TABLE;
    pgd[1] = V2P(pmd_1) | PTE_VALID | PTE_TABLE;
    pgd[2] = V2P(pmd_2) | PTE_VALID | PTE_TABLE;
    pgd[3] = V2P(pmd_3) | PTE_VALID | PTE_TABLE;

    for (unsigned long i = 0; i < 512; i++)
    {
        unsigned long* l3_table = alloc_table_page();
        pmd_0[i] = V2P(l3_table) | PTE_VALID | PTE_TABLE;

        for (unsigned long j = 0; j < 512; j++)
        {
            unsigned long addr = (i * 2 * 1024 * 1024ULL) + (j * 4096ULL);
            l3_table[j] = addr | PTE_VALID | PTE_PAGE | PTE_AF | PTE_SH_INNER | PTE_ATTR_NORMAL | PTE_AP_RW;
        }
    }

    unsigned long* pmd_others[] = {pmd_1, pmd_2, pmd_3};
    for (unsigned long p = 0; p < 3; p++)
    {
        for (unsigned long i = 0; i < 512; i++)
        {
            unsigned long addr = ((p + 1) * 1024ULL * 1024ULL * 1024ULL) + (i * 2 * 1024 * 1024ULL);
            pmd_others[p][i] = addr | PTE_VALID | PTE_BLOCK | PTE_AF | PTE_PXN | PTE_UXN | PTE_AP_RW | PTE_ATTR_DEVICE;
        }
    }

    kernel_pgd_virt = pgd;
    kernel_pgd_phys = V2P(pgd);

    // Map kernel segments with specific page permissions
    unsigned long k_start = (unsigned long)__text_start;
    unsigned long k_end = pmm_metadata_end;

    for (unsigned long vaddr = k_start; vaddr < k_end; vaddr += PAGE_SIZE)
    {
        unsigned long paddr = V2P(vaddr);
        unsigned long attr = PTE_VALID | PTE_PAGE | PTE_AF | PTE_SH_INNER | PTE_ATTR_NORMAL;

        if (vaddr >= (unsigned long)__text_start && vaddr < (unsigned long)__text_end)
        {
            attr |= PTE_AP_RO;
        }
        else if (vaddr >= (unsigned long)__rodata_start && vaddr < (unsigned long)__rodata_end)
        {
            attr |= PTE_AP_RO | PTE_PXN | PTE_UXN;
        }
        else
        {
            attr |= PTE_AP_RW | PTE_PXN | PTE_UXN;
        }

        mmu_map_page(vaddr, paddr, attr);
    }

    // Allocate a zeroed PGD for TTBR0 to trap unhandled user accesses
    unsigned long* empty_pgd = (unsigned long*)pmm_alloc_page();
    memset(empty_pgd, 0, PAGE_SIZE);
    empty_pgd_phys = V2P(empty_pgd);

    // Load page table roots into the system registers
    asm volatile("msr ttbr1_el1, %0" : : "r"(kernel_pgd_phys));
    asm volatile("msr ttbr0_el1, %0" : : "r"(empty_pgd_phys));

    asm volatile("tlbi vmalle1is");
    asm volatile("dsb ish");
    asm volatile("isb");

    printf("[  MMU ] TTBR1 -> 0x%lx, TTBR0 -> empty\n", kernel_pgd_phys);
    printf("[  MMU ] Mapped: 4 GB range (Blocks + Kernel Pages)\n");
}

/*
 * mmu_secondary_init - Secondary core memory initialization.
 */
void mmu_secondary_init(void)
{
    asm volatile("msr ttbr1_el1, %0" : : "r"(kernel_pgd_phys));
    asm volatile("msr ttbr0_el1, %0" : : "r"(empty_pgd_phys));

    asm volatile("tlbi vmalle1is");
    asm volatile("dsb ish");
    asm volatile("isb");
}

/*
 * split_block_to_pages - Splits a 2MB block descriptor into 512 4KB pages.
 * Implements Break-Before-Make to ensure TLB consistency.
 */
static unsigned long* split_block_to_pages(unsigned long* l2_table, unsigned long l2_idx, unsigned long vaddr_base)
{
    unsigned long block_entry = l2_table[l2_idx];
    unsigned long block_phys = block_entry & PTE_ADDR_MASK;
    unsigned long block_attr = block_entry & ~PTE_ADDR_MASK & ~PTE_TABLE;

    unsigned long* l3_table = alloc_table_page();

    for (unsigned long i = 0; i < 512; i++)
    {
        unsigned long page_phys = block_phys + i * PAGE_SIZE;
        l3_table[i] = page_phys | PTE_VALID | PTE_PAGE | block_attr;
    }

    l2_table[l2_idx] = 0;
    for (unsigned long v = vaddr_base; v < vaddr_base + 0x200000ULL; v += PAGE_SIZE)
    {
        tlbi_va(v);
    }

    l2_table[l2_idx] = V2P(l3_table) | PTE_VALID | PTE_TABLE;

    asm volatile("dsb ish");
    asm volatile("isb");

    return l3_table;
}

/*
 * mmu_map_page - Mapping implementation for the kernel address space.
 */
void mmu_map_page(unsigned long vaddr, unsigned long paddr, unsigned long flags)
{
    unsigned long irqflags = spin_lock_irqsave(&mmu_lock);

    unsigned long l1_idx = L1_INDEX(vaddr);
    unsigned long l2_idx = L2_INDEX(vaddr);
    unsigned long l3_idx = L3_INDEX(vaddr);

    unsigned long l1_entry = kernel_pgd_virt[l1_idx];
    unsigned long* l2_table;
    if (l1_entry & PTE_VALID)
    {
        l2_table = (unsigned long*)P2V(l1_entry & PTE_ADDR_MASK);
    }
    else
    {
        l2_table = alloc_table_page();
        kernel_pgd_virt[l1_idx] = V2P(l2_table) | PTE_VALID | PTE_TABLE;
    }

    unsigned long l2_entry = l2_table[l2_idx];
    unsigned long* l3_table;

    if (l2_entry & PTE_VALID)
    {
        if (!(l2_entry & PTE_TABLE))
        {
            unsigned long vaddr_base = vaddr & ~0x1FFFFFULL;
            l3_table = split_block_to_pages(l2_table, l2_idx, vaddr_base);
        }
        else
        {
            l3_table = (unsigned long*)P2V(l2_entry & PTE_ADDR_MASK);
        }
    }
    else
    {
        l3_table = alloc_table_page();
        l2_table[l2_idx] = V2P(l3_table) | PTE_VALID | PTE_TABLE;
    }

    if (l3_table[l3_idx] & PTE_VALID)
    {
        unsigned long old_pa = l3_table[l3_idx] & PTE_ADDR_MASK;
        l3_table[l3_idx] = 0;
        tlbi_va(vaddr);
        pmm_free_page((void*)P2V(old_pa));
    }

    l3_table[l3_idx] = (paddr & PTE_ADDR_MASK) | PTE_VALID | PTE_PAGE | flags;
    pmm_hold_page((void*)P2V(paddr & PTE_ADDR_MASK));
    tlbi_va(vaddr);

    spin_unlock_irqrestore(&mmu_lock, irqflags);
}

/*
 * mmu_unmap_page - Unmapping implementation for the kernel address space.
 */
void mmu_unmap_page(unsigned long vaddr)
{
    unsigned long irqflags = spin_lock_irqsave(&mmu_lock);

    unsigned long l1_idx = L1_INDEX(vaddr);
    unsigned long l2_idx = L2_INDEX(vaddr);
    unsigned long l3_idx = L3_INDEX(vaddr);

    unsigned long l1_entry = kernel_pgd_virt[l1_idx];
    if (!(l1_entry & PTE_VALID))
    {
        spin_unlock_irqrestore(&mmu_lock, irqflags);
        return;
    }
    unsigned long* l2_table = (unsigned long*)P2V(l1_entry & PTE_ADDR_MASK);

    unsigned long l2_entry = l2_table[l2_idx];
    if (!(l2_entry & PTE_VALID))
    {
        spin_unlock_irqrestore(&mmu_lock, irqflags);
        return;
    }

    unsigned long* l3_table;
    if (!(l2_entry & PTE_TABLE))
    {
        unsigned long vaddr_base = vaddr & ~0x1FFFFFULL;
        l3_table = split_block_to_pages(l2_table, l2_idx, vaddr_base);
    }
    else
    {
        l3_table = (unsigned long*)P2V(l2_entry & PTE_ADDR_MASK);
    }

    if (l3_table[l3_idx] & PTE_VALID)
    {
        l3_table[l3_idx] = 0;
        tlbi_va(vaddr);
    }

    spin_unlock_irqrestore(&mmu_lock, irqflags);
}

/*
 * mmu_query - Mapping query implementation.
 */
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

    if (!(l2_entry & PTE_TABLE))
    {
        unsigned long block_phys = l2_entry & PTE_ADDR_MASK;
        unsigned long offset = vaddr & 0x1FFFFFULL;
        if (out_paddr)
        {
            *out_paddr = block_phys + offset;
        }
        if (out_flags)
        {
            *out_flags = l2_entry & ~PTE_ADDR_MASK;
        }
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
    {
        *out_paddr = (l3_entry & PTE_ADDR_MASK) + PAGE_OFFSET(vaddr);
    }
    if (out_flags)
    {
        *out_flags = l3_entry & ~PTE_ADDR_MASK;
    }

    spin_unlock_irqrestore(&mmu_lock, irqflags);
    return 1;
}

/*
 * mmu_create_user_pgd - Allocates a top-level user page table.
 */
unsigned long* mmu_create_user_pgd(void)
{
    return alloc_table_page();
}

/*
 * mmu_destroy_user_pgd - Recursively destroys a user address space.
 */
void mmu_destroy_user_pgd(unsigned long* pgd)
{
    for (int i = 0; i < 512; i++)
    {
        if (!(pgd[i] & PTE_VALID) || !(pgd[i] & PTE_TABLE))
        {
            continue;
        }

        unsigned long* l2 = (unsigned long*)P2V(pgd[i] & PTE_ADDR_MASK);
        for (int j = 0; j < 512; j++)
        {
            if (!(l2[j] & PTE_VALID))
            {
                continue;
            }
            if (l2[j] & PTE_TABLE)
            {
                unsigned long* l3 = (unsigned long*)P2V(l2[j] & PTE_ADDR_MASK);
                for (int k = 0; k < 512; k++)
                {
                    if ((l3[k] & PTE_VALID) && (l3[k] & PTE_PAGE))
                    {
                        unsigned long pa = l3[k] & PTE_ADDR_MASK;
                        pmm_free_page((void*)P2V(pa));
                    }
                }
                pmm_free_page(l3);
            }
        }
        pmm_free_page(l2);
    }
    pmm_free_page(pgd);
}

/*
 * mmu_copy_user_pgd - Deep copy of a user address space with CoW semantics.
 */
unsigned long* mmu_copy_user_pgd(unsigned long* parent_pgd)
{
    unsigned long* child_pgd = mmu_create_user_pgd();
    if (!child_pgd)
    {
        return (void*)0;
    }

    unsigned long flags = spin_lock_irqsave(&mmu_lock);

    for (int i = 0; i < 512; i++)
    {
        if (!(parent_pgd[i] & MMU_PTE_VALID))
        {
            continue;
        }

        unsigned long* parent_pmd = (unsigned long*)P2V(parent_pgd[i] & PTE_ADDR_MASK);
        unsigned long* child_pmd = alloc_table_page();
        child_pgd[i] = V2P(child_pmd) | MMU_PTE_VALID | MMU_PTE_TABLE;

        for (int j = 0; j < 512; j++)
        {
            if (!(parent_pmd[j] & MMU_PTE_VALID))
            {
                continue;
            }

            if (parent_pmd[j] & MMU_PTE_TABLE)
            {
                unsigned long* parent_pte = (unsigned long*)P2V(parent_pmd[j] & PTE_ADDR_MASK);
                unsigned long* child_pte = alloc_table_page();
                child_pmd[j] = V2P(child_pte) | MMU_PTE_VALID | MMU_PTE_TABLE;

                for (int k = 0; k < 512; k++)
                {
                    if (!(parent_pte[k] & MMU_PTE_VALID))
                    {
                        continue;
                    }

                    unsigned long pte = parent_pte[k];

                    // Convert RW pages to RO and mark as CoW
                    if ((pte & (3ULL << 6)) == MMU_AP_RW)
                    {
                        pte &= ~(3ULL << 6);
                        pte |= MMU_AP_RO;
                        pte |= MMU_PTE_COW;
                        parent_pte[k] = pte;
                    }

                    child_pte[k] = pte;
                    pmm_hold_page((void*)P2V(pte & PTE_ADDR_MASK));
                }
            }
        }
    }

    asm volatile("tlbi vmalle1is\n dsb ish\n isb");
    spin_unlock_irqrestore(&mmu_lock, flags);
    return child_pgd;
}

/*
 * mmu_user_map_page - Established a 4KB mapping in a user address space.
 */
void mmu_user_map_page(unsigned long* pgd, unsigned long vaddr, unsigned long paddr, unsigned long flags)
{
    unsigned long irqflags = spin_lock_irqsave(&mmu_lock);

    unsigned long l1_idx = L1_INDEX(vaddr);
    unsigned long l2_idx = L2_INDEX(vaddr);
    unsigned long l3_idx = L3_INDEX(vaddr);

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

    unsigned long* l3_table;
    if (l2_table[l2_idx] & PTE_VALID)
    {
        l3_table = (unsigned long*)P2V(l2_table[l2_idx] & PTE_ADDR_MASK);
    }
    else
    {
        l3_table = alloc_table_page();
        l2_table[l2_idx] = V2P(l3_table) | PTE_VALID | PTE_TABLE;
    }

    if (l3_table[l3_idx] & PTE_VALID)
    {
        unsigned long old_pa = l3_table[l3_idx] & PTE_ADDR_MASK;
        l3_table[l3_idx] = 0;
        tlbi_va(vaddr);
        pmm_free_page((void*)P2V(old_pa));
    }

    l3_table[l3_idx] = (paddr & PTE_ADDR_MASK) | PTE_VALID | PTE_PAGE | flags;
    tlbi_va(vaddr);

    spin_unlock_irqrestore(&mmu_lock, irqflags);
}

/*
 * mmu_user_unmap_page - Removes a mapping from a user address space.
 */
void mmu_user_unmap_page(unsigned long* pgd, unsigned long vaddr)
{
    unsigned long irqflags = spin_lock_irqsave(&mmu_lock);

    unsigned long l1_idx = L1_INDEX(vaddr);
    unsigned long l2_idx = L2_INDEX(vaddr);
    unsigned long l3_idx = L3_INDEX(vaddr);

    if (!(pgd[l1_idx] & PTE_VALID))
    {
        spin_unlock_irqrestore(&mmu_lock, irqflags);
        return;
    }
    unsigned long* l2 = (unsigned long*)P2V(pgd[l1_idx] & PTE_ADDR_MASK);

    if (!(l2[l2_idx] & PTE_VALID) || !(l2[l2_idx] & PTE_TABLE))
    {
        spin_unlock_irqrestore(&mmu_lock, irqflags);
        return;
    }
    unsigned long* l3 = (unsigned long*)P2V(l2[l2_idx] & PTE_ADDR_MASK);

    if (l3[l3_idx] & PTE_VALID)
    {
        unsigned long old_pa = l3[l3_idx] & PTE_ADDR_MASK;
        l3[l3_idx] = 0;
        tlbi_va(vaddr);
        pmm_free_page((void*)P2V(old_pa));
    }

    spin_unlock_irqrestore(&mmu_lock, irqflags);
}

/*
 * mmu_switch_user - Switches the current TTBR0 to a new user PGD.
 */
void mmu_switch_user(unsigned long* pgd, unsigned long asid)
{
    unsigned long ttbr0 = V2P(pgd) | (asid << 48);
    asm volatile("msr ttbr0_el1, %0" : : "r"(ttbr0));
    asm volatile("dsb sy");
    asm volatile("isb");
}

/*
 * mmu_user_query - Query implementation for user address spaces.
 */
int mmu_user_query(unsigned long* pgd, unsigned long vaddr, unsigned long* out_paddr, unsigned long* out_flags)
{
    if (!pgd)
    {
        return 0;
    }

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
        unsigned long block_phys = l2[l2_idx] & PTE_ADDR_MASK;
        unsigned long offset = vaddr & 0x1FFFFFULL;
        if (out_paddr)
        {
            *out_paddr = block_phys + offset;
        }
        if (out_flags)
        {
            *out_flags = l2[l2_idx] & ~PTE_ADDR_MASK;
        }
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
    {
        *out_paddr = (l3[l3_idx] & PTE_ADDR_MASK) + PAGE_OFFSET(vaddr);
    }
    if (out_flags)
    {
        *out_flags = l3[l3_idx] & ~PTE_ADDR_MASK;
    }

    spin_unlock_irqrestore(&mmu_lock, irqflags);
    return 1;
}

/*
 * mmu_handle_cow - Resolution of Copy-on-Write page faults.
 */
int mmu_handle_cow(unsigned long* pgd, unsigned long vaddr)
{
    vaddr &= ~0xFFFULL;
    unsigned long paddr, flags;

    if (!mmu_user_query(pgd, vaddr, &paddr, &flags))
    {
        return -1;
    }

    if (!(flags & MMU_PTE_COW))
    {
        return -1;
    }

    void* new_page = pmm_alloc_page();
    if (!new_page)
    {
        return -1;
    }

    memcpy(new_page, (void*)P2V(paddr), PAGE_SIZE);

    unsigned long new_flags = (flags & ~MMU_PTE_COW);
    new_flags &= ~(3ULL << 6);
    new_flags |= MMU_AP_RW;

    mmu_user_map_page(pgd, vaddr, V2P(new_page), new_flags);
    pmm_free_page((void*)P2V(paddr));
    return 0;
}

/*
 * mmu_kernel_ttbr0 - Returns the empty PGD root for kernel-only threads.
 */
unsigned long mmu_kernel_ttbr0(void)
{
    return empty_pgd_phys;
}
