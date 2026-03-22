/*
 * mmu.c - AArch64 Memory Management Unit driver.
 *
 * Implements page table management for 4 KB / 3-level (39-bit VA) config.
 */

#include "mm/mmu.h"

#include "mm/pmm.h"
#include "mm/addr.h"
#include "core/lock.h"
#include "stdio.h"
#include "string.h"
#include "panic.h"

/* Internal shorthands */
#define PTE_VALID    MMU_PTE_VALID
#define PTE_TABLE    MMU_PTE_TABLE
#define PTE_PAGE     MMU_PTE_PAGE
#define PTE_BLOCK    MMU_PTE_BLOCK
#define PTE_AF       MMU_PTE_AF
#define PTE_SH_INNER MMU_PTE_SH_INNER
#define PTE_PXN      MMU_PXN
#define PTE_UXN      MMU_UXN
#define PTE_AP_RW    MMU_AP_RW
#define PTE_AP_RO    MMU_AP_RO
#define PTE_ATTR_N   MMU_ATTR_NORMAL
#define PTE_ATTR_D   MMU_ATTR_DEVICE
#define PTE_ADDR     MMU_PTE_ADDR_MASK

/* VA decomposition for 39-bit / 4 KB granule */
#define L1_IDX(va) (((va) >> 30) & 0x1FFUL)
#define L2_IDX(va) (((va) >> 21) & 0x1FFUL)
#define L3_IDX(va) (((va) >> 12) & 0x1FFUL)
#define PG_OFF(va) ((va) & 0xFFFUL)

#define PT_ENTRIES 512UL

extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], __data_end[];
extern char __bss_start[], __bss_end[];
extern char __kernel_end[];
extern unsigned long pmm_metadata_end;

static unsigned long kernel_pgd_phys;
static unsigned long* kernel_pgd_virt;
static unsigned long empty_pgd_phys;

static spinlock_t mmu_lock = SPINLOCK_INIT;

/* Invalidate TLB entry for a single page (Inner Shareable) */
static inline void tlbi_va_is(unsigned long vaddr)
{
    unsigned long va_op = vaddr >> 12;
    asm volatile("dsb ishst" : : : "memory");
    asm volatile("tlbi vale1is, %0" : : "r"(va_op));
    asm volatile("dsb ish" : : : "memory");
    asm volatile("isb" : : : "memory");
}

/* Invalidate all TLB entries (Inner Shareable) */
static inline void tlbi_all_is(void)
{
    asm volatile("dsb ishst" : : : "memory");
    asm volatile("tlbi vmalle1is" : : : "memory");
    asm volatile("dsb ish" : : : "memory");
    asm volatile("isb" : : : "memory");
}

/* Allocate and zero a page for use as a page table (panics on OOM) */
static unsigned long* alloc_table_page(void)
{
    unsigned long* page = (unsigned long*)pmm_alloc_page();
    if (!page)
    {
        PANIC("MMU: out of memory for page table");
    }
    memset(page, 0, PAGE_SIZE);
    return page;
}

/* Allocate and zero a page table for user space (returns NULL on OOM) */
static unsigned long* alloc_user_table_page(void)
{
    unsigned long* page = (unsigned long*)pmm_alloc_page();
    if (!page)
    {
        return NULL;
    }
    memset(page, 0, PAGE_SIZE);
    return page;
}

void mmu_init(void)
{
    unsigned long* pgd = alloc_table_page();
    unsigned long* pmd_0 = alloc_table_page(); /* 0–1 GB  */
    unsigned long* pmd_1 = alloc_table_page(); /* 1–2 GB  */
    unsigned long* pmd_2 = alloc_table_page(); /* 2–3 GB  */
    unsigned long* pmd_3 = alloc_table_page(); /* 3–4 GB  */

    pgd[0] = V2P((uintptr_t)pmd_0) | PTE_VALID | PTE_TABLE;
    pgd[1] = V2P((uintptr_t)pmd_1) | PTE_VALID | PTE_TABLE;
    pgd[2] = V2P((uintptr_t)pmd_2) | PTE_VALID | PTE_TABLE;
    pgd[3] = V2P((uintptr_t)pmd_3) | PTE_VALID | PTE_TABLE;

    // First GB: fine-grained 4 KB pages
    for (unsigned long i = 0; i < PT_ENTRIES; i++)
    {
        unsigned long* l3 = alloc_table_page();
        pmd_0[i] = V2P((uintptr_t)l3) | PTE_VALID | PTE_TABLE;

        for (unsigned long j = 0; j < PT_ENTRIES; j++)
        {
            unsigned long pa = (i * (2UL * 1024 * 1024)) + (j * PAGE_SIZE);
            l3[j] = pa | PTE_VALID | PTE_PAGE | PTE_AF | PTE_SH_INNER | PTE_ATTR_N | PTE_AP_RW | PTE_PXN | PTE_UXN;
        }
    }

    // GB 1-3: Device 2 MB blocks
    unsigned long* pmd_others[3] = {pmd_1, pmd_2, pmd_3};
    for (unsigned long p = 0; p < 3; p++)
    {
        for (unsigned long i = 0; i < PT_ENTRIES; i++)
        {
            unsigned long pa = ((p + 1) * (1024UL * 1024 * 1024)) + (i * (2UL * 1024 * 1024));
            pmd_others[p][i] = pa | PTE_VALID | PTE_BLOCK | PTE_AF | PTE_PXN | PTE_UXN | PTE_AP_RW | PTE_ATTR_D;
        }
    }

    kernel_pgd_virt = pgd;
    kernel_pgd_phys = V2P((uintptr_t)pgd);

    // Precise permissions for kernel segments
    unsigned long ks = (unsigned long)__text_start;
    unsigned long ke = (unsigned long)pmm_metadata_end;

    for (unsigned long va = ks; va < ke; va += PAGE_SIZE)
    {
        unsigned long pa = V2P(va);
        unsigned long attr = PTE_VALID | PTE_PAGE | PTE_AF | PTE_SH_INNER | PTE_ATTR_N;

        if (va >= (unsigned long)__text_start && va < (unsigned long)__text_end)
        {
            attr |= PTE_AP_RO | PTE_UXN;
        }
        else if (va >= (unsigned long)__rodata_start && va < (unsigned long)__rodata_end)
        {
            attr |= PTE_AP_RO | PTE_PXN | PTE_UXN;
        }
        else
        {
            attr |= PTE_AP_RW | PTE_PXN | PTE_UXN;
        }

        mmu_map_page(va, pa, attr);
    }

    unsigned long* empty_pgd = alloc_table_page();
    empty_pgd_phys = V2P((uintptr_t)empty_pgd);

    asm volatile("msr ttbr1_el1, %0" : : "r"(kernel_pgd_phys) : "memory");
    asm volatile("msr ttbr0_el1, %0" : : "r"(empty_pgd_phys) : "memory");
    tlbi_all_is();

    printf("[  MMU ] TTBR1 -> 0x%lx, TTBR0 -> empty (0x%lx)\n", kernel_pgd_phys, empty_pgd_phys);
    printf("[  MMU ] Kernel mapped [0x%lx – 0x%lx)\n", ks, ke);
}

void mmu_secondary_init(void)
{
    asm volatile("msr ttbr1_el1, %0" : : "r"(kernel_pgd_phys) : "memory");
    asm volatile("msr ttbr0_el1, %0" : : "r"(empty_pgd_phys) : "memory");
    tlbi_all_is();
}

/* Replace a 2 MB L2 block with 512 × 4 KB page descriptors (BBM) */
static unsigned long* split_block_to_pages(unsigned long* l2_table, unsigned long l2_idx, unsigned long vaddr_base)
{
    unsigned long block_entry = l2_table[l2_idx];
    unsigned long block_phys = block_entry & PTE_ADDR;
    unsigned long block_attr = block_entry & ~PTE_ADDR & ~PTE_TABLE;

    unsigned long* l3 = alloc_table_page();

    for (unsigned long i = 0; i < PT_ENTRIES; i++)
    {
        unsigned long page_phys = block_phys + i * PAGE_SIZE;
        l3[i] = page_phys | PTE_VALID | PTE_PAGE | block_attr;
    }

    l2_table[l2_idx] = 0;
    asm volatile("dsb ishst" : : : "memory");

    for (unsigned long v = vaddr_base; v < vaddr_base + (2UL * 1024 * 1024); v += PAGE_SIZE)
    {
        tlbi_va_is(v);
    }

    l2_table[l2_idx] = V2P((uintptr_t)l3) | PTE_VALID | PTE_TABLE;
    asm volatile("dsb ish" : : : "memory");
    asm volatile("isb" : : : "memory");

    return l3;
}

void mmu_map_page(unsigned long vaddr, unsigned long paddr, unsigned long flags)
{
    paddr &= PTE_ADDR;
    unsigned long irq = spin_lock_irqsave(&mmu_lock);

    unsigned long l1 = L1_IDX(vaddr);
    unsigned long l2 = L2_IDX(vaddr);
    unsigned long l3 = L3_IDX(vaddr);

    unsigned long* l2t;
    if (kernel_pgd_virt[l1] & PTE_VALID)
    {
        l2t = (unsigned long*)P2V(kernel_pgd_virt[l1] & PTE_ADDR);
    }
    else
    {
        l2t = alloc_table_page();
        kernel_pgd_virt[l1] = V2P((uintptr_t)l2t) | PTE_VALID | PTE_TABLE;
    }

    unsigned long* l3t;
    unsigned long l2e = l2t[l2];

    if (l2e & PTE_VALID)
    {
        if (!(l2e & PTE_TABLE))
        {
            unsigned long base = vaddr & ~((2UL * 1024 * 1024) - 1);
            l3t = split_block_to_pages(l2t, l2, base);
        }
        else
        {
            l3t = (unsigned long*)P2V(l2e & PTE_ADDR);
        }
    }
    else
    {
        l3t = alloc_table_page();
        l2t[l2] = V2P((uintptr_t)l3t) | PTE_VALID | PTE_TABLE;
    }

    if (l3t[l3] & PTE_VALID)
    {
        l3t[l3] = 0;
        tlbi_va_is(vaddr);
    }

    l3t[l3] = paddr | PTE_VALID | PTE_PAGE | flags;
    tlbi_va_is(vaddr);

    spin_unlock_irqrestore(&mmu_lock, irq);
}

void mmu_unmap_page(unsigned long vaddr)
{
    unsigned long irq = spin_lock_irqsave(&mmu_lock);

    unsigned long l1 = L1_IDX(vaddr);
    unsigned long l2 = L2_IDX(vaddr);
    unsigned long l3 = L3_IDX(vaddr);

    if (!(kernel_pgd_virt[l1] & PTE_VALID))
        goto out;

    unsigned long* l2t = (unsigned long*)P2V(kernel_pgd_virt[l1] & PTE_ADDR);
    unsigned long l2e = l2t[l2];

    if (!(l2e & PTE_VALID))
        goto out;

    unsigned long* l3t;
    if (!(l2e & PTE_TABLE))
    {
        unsigned long base = vaddr & ~((2UL * 1024 * 1024) - 1);
        l3t = split_block_to_pages(l2t, l2, base);
    }
    else
    {
        l3t = (unsigned long*)P2V(l2e & PTE_ADDR);
    }

    if (l3t[l3] & PTE_VALID)
    {
        l3t[l3] = 0;
        tlbi_va_is(vaddr);
    }

out:
    spin_unlock_irqrestore(&mmu_lock, irq);
}

int mmu_query(unsigned long vaddr, unsigned long* out_paddr, unsigned long* out_flags)
{
    unsigned long irq = spin_lock_irqsave(&mmu_lock);
    int found = 0;

    unsigned long l1 = L1_IDX(vaddr);
    unsigned long l2 = L2_IDX(vaddr);
    unsigned long l3 = L3_IDX(vaddr);

    if (!(kernel_pgd_virt[l1] & PTE_VALID))
        goto out;

    unsigned long* l2t = (unsigned long*)P2V(kernel_pgd_virt[l1] & PTE_ADDR);
    unsigned long l2e = l2t[l2];

    if (!(l2e & PTE_VALID))
        goto out;

    if (!(l2e & PTE_TABLE))
    {
        if (out_paddr)
            *out_paddr = (l2e & PTE_ADDR) + (vaddr & 0x1FFFFFUL);
        if (out_flags)
            *out_flags = l2e & ~PTE_ADDR;
        found = 1;
        goto out;
    }

    unsigned long* l3t = (unsigned long*)P2V(l2e & PTE_ADDR);
    unsigned long l3e = l3t[l3];

    if (!(l3e & PTE_VALID))
        goto out;

    if (out_paddr)
        *out_paddr = (l3e & PTE_ADDR) + PG_OFF(vaddr);
    if (out_flags)
        *out_flags = l3e & ~PTE_ADDR;
    found = 1;

out:
    spin_unlock_irqrestore(&mmu_lock, irq);
    return found;
}

unsigned long* mmu_create_user_pgd(void)
{
    return alloc_user_table_page();
}

void mmu_destroy_user_pgd(unsigned long* pgd)
{
    if (!pgd)
        return;

    for (unsigned long i = 0; i < PT_ENTRIES; i++)
    {
        unsigned long l1e = pgd[i];
        if (!(l1e & PTE_VALID) || !(l1e & PTE_TABLE))
            continue;

        unsigned long* l2 = (unsigned long*)P2V(l1e & PTE_ADDR);

        for (unsigned long j = 0; j < PT_ENTRIES; j++)
        {
            unsigned long l2e = l2[j];
            if (!(l2e & PTE_VALID))
                continue;

            if (!(l2e & PTE_TABLE))
            {
                pmm_free_page((void*)P2V(l2e & PTE_ADDR));
                continue;
            }

            unsigned long* l3 = (unsigned long*)P2V(l2e & PTE_ADDR);
            for (unsigned long k = 0; k < PT_ENTRIES; k++)
            {
                unsigned long l3e = l3[k];
                if ((l3e & PTE_VALID) && (l3e & PTE_PAGE))
                {
                    pmm_free_page((void*)P2V(l3e & PTE_ADDR));
                }
            }
            pmm_free_page(l3);
        }
        pmm_free_page(l2);
    }
    pmm_free_page(pgd);
}

unsigned long* mmu_copy_user_pgd(unsigned long* parent_pgd)
{
    if (!parent_pgd)
        return NULL;

    unsigned long* child_pgd = mmu_create_user_pgd();
    if (!child_pgd)
        return NULL;

    unsigned long irq = spin_lock_irqsave(&mmu_lock);

    for (unsigned long i = 0; i < PT_ENTRIES; i++)
    {
        unsigned long l1e = parent_pgd[i];
        if (!(l1e & MMU_PTE_VALID))
            continue;

        unsigned long* parent_pmd = (unsigned long*)P2V(l1e & PTE_ADDR);
        unsigned long* child_pmd = alloc_user_table_page();
        if (!child_pmd)
            goto oom;

        child_pgd[i] = V2P((uintptr_t)child_pmd) | MMU_PTE_VALID | MMU_PTE_TABLE;

        for (unsigned long j = 0; j < PT_ENTRIES; j++)
        {
            unsigned long l2e = parent_pmd[j];
            if (!(l2e & MMU_PTE_VALID))
                continue;

            if (!(l2e & MMU_PTE_TABLE))
            {
                unsigned long pte = l2e;
                if ((pte & (3ULL << 6)) == MMU_AP_RW)
                {
                    pte &= ~(3ULL << 6);
                    pte |= MMU_AP_RO | MMU_PTE_COW;
                    parent_pmd[j] = pte;
                }
                child_pmd[j] = pte;
                pmm_hold_page((void*)P2V(pte & PTE_ADDR));
                continue;
            }

            unsigned long* parent_pte = (unsigned long*)P2V(l2e & PTE_ADDR);
            unsigned long* child_pte = alloc_user_table_page();
            if (!child_pte)
                goto oom;

            child_pmd[j] = V2P((uintptr_t)child_pte) | MMU_PTE_VALID | MMU_PTE_TABLE;

            for (unsigned long k = 0; k < PT_ENTRIES; k++)
            {
                unsigned long pte = parent_pte[k];
                if (!(pte & MMU_PTE_VALID))
                    continue;

                if ((pte & (3ULL << 6)) == MMU_AP_RW)
                {
                    pte &= ~(3ULL << 6);
                    pte |= MMU_AP_RO | MMU_PTE_COW;
                    parent_pte[k] = pte;
                }
                child_pte[k] = pte;
                pmm_hold_page((void*)P2V(pte & PTE_ADDR));
            }
        }
    }

    tlbi_all_is();
    spin_unlock_irqrestore(&mmu_lock, irq);
    return child_pgd;

oom:
    spin_unlock_irqrestore(&mmu_lock, irq);
    return child_pgd;
}

void mmu_user_map_page(unsigned long* pgd, unsigned long vaddr, unsigned long paddr, unsigned long flags)
{
    if (!pgd)
    {
        PANIC("mmu_user_map_page: NULL pgd");
    }

    paddr &= PTE_ADDR;
    unsigned long irq = spin_lock_irqsave(&mmu_lock);

    unsigned long l1 = L1_IDX(vaddr);
    unsigned long l2 = L2_IDX(vaddr);
    unsigned long l3 = L3_IDX(vaddr);

    unsigned long* l2t;
    if (pgd[l1] & PTE_VALID)
    {
        l2t = (unsigned long*)P2V(pgd[l1] & PTE_ADDR);
    }
    else
    {
        l2t = alloc_user_table_page();
        if (!l2t)
        {
            spin_unlock_irqrestore(&mmu_lock, irq);
            return;
        }
        pgd[l1] = V2P((uintptr_t)l2t) | PTE_VALID | PTE_TABLE;
    }

    unsigned long* l3t;
    if (l2t[l2] & PTE_VALID)
    {
        if (!(l2t[l2] & PTE_TABLE))
        {
            unsigned long base = vaddr & ~((2UL * 1024 * 1024) - 1);
            l3t = split_block_to_pages(l2t, l2, base);
        }
        else
        {
            l3t = (unsigned long*)P2V(l2t[l2] & PTE_ADDR);
        }
    }
    else
    {
        l3t = alloc_user_table_page();
        if (!l3t)
        {
            spin_unlock_irqrestore(&mmu_lock, irq);
            return;
        }
        l2t[l2] = V2P((uintptr_t)l3t) | PTE_VALID | PTE_TABLE;
    }

    if (l3t[l3] & PTE_VALID)
    {
        unsigned long old_pa = l3t[l3] & PTE_ADDR;
        l3t[l3] = 0;
        tlbi_va_is(vaddr);
        pmm_free_page((void*)P2V(old_pa));
    }

    l3t[l3] = paddr | PTE_VALID | PTE_PAGE | flags;
    pmm_hold_page((void*)P2V(paddr));
    tlbi_va_is(vaddr);

    spin_unlock_irqrestore(&mmu_lock, irq);
}

void mmu_user_unmap_page(unsigned long* pgd, unsigned long vaddr)
{
    if (!pgd)
        return;

    unsigned long irq = spin_lock_irqsave(&mmu_lock);

    unsigned long l1 = L1_IDX(vaddr);
    unsigned long l2 = L2_IDX(vaddr);
    unsigned long l3 = L3_IDX(vaddr);

    if (!(pgd[l1] & PTE_VALID))
        goto out;

    unsigned long* l2t = (unsigned long*)P2V(pgd[l1] & PTE_ADDR);
    unsigned long l2e = l2t[l2];

    if (!(l2e & PTE_VALID))
        goto out;

    unsigned long* l3t;
    if (!(l2e & PTE_TABLE))
    {
        unsigned long base = vaddr & ~((2UL * 1024 * 1024) - 1);
        l3t = split_block_to_pages(l2t, l2, base);
    }
    else
    {
        l3t = (unsigned long*)P2V(l2e & PTE_ADDR);
    }

    if (l3t[l3] & PTE_VALID)
    {
        unsigned long old_pa = l3t[l3] & PTE_ADDR;
        l3t[l3] = 0;
        tlbi_va_is(vaddr);
        pmm_free_page((void*)P2V(old_pa));
    }

out:
    spin_unlock_irqrestore(&mmu_lock, irq);
}

void mmu_switch_user(unsigned long* pgd, unsigned long asid)
{
    unsigned long safe_asid = asid & 0xFFUL;
    unsigned long ttbr0 = V2P((uintptr_t)pgd) | (safe_asid << 48);

    asm volatile("msr ttbr0_el1, %0" : : "r"(ttbr0) : "memory");
    asm volatile("dsb ish" : : : "memory");
    asm volatile("isb" : : : "memory");
}

int mmu_user_query(unsigned long* pgd, unsigned long vaddr, unsigned long* out_paddr, unsigned long* out_flags)
{
    if (!pgd)
        return 0;

    unsigned long irq = spin_lock_irqsave(&mmu_lock);
    int found = 0;

    unsigned long l1 = L1_IDX(vaddr);
    unsigned long l2 = L2_IDX(vaddr);
    unsigned long l3 = L3_IDX(vaddr);

    if (!(pgd[l1] & PTE_VALID))
        goto out;

    unsigned long* l2t = (unsigned long*)P2V(pgd[l1] & PTE_ADDR);
    unsigned long l2e = l2t[l2];

    if (!(l2e & PTE_VALID))
        goto out;

    if (!(l2e & PTE_TABLE))
    {
        if (out_paddr)
            *out_paddr = (l2e & PTE_ADDR) + (vaddr & 0x1FFFFFUL);
        if (out_flags)
            *out_flags = l2e & ~PTE_ADDR;
        found = 1;
        goto out;
    }

    unsigned long* l3t = (unsigned long*)P2V(l2e & PTE_ADDR);
    unsigned long l3e = l3t[l3];

    if (!(l3e & PTE_VALID))
        goto out;

    if (out_paddr)
        *out_paddr = (l3e & PTE_ADDR) + PG_OFF(vaddr);
    if (out_flags)
        *out_flags = l3e & ~PTE_ADDR;
    found = 1;

out:
    spin_unlock_irqrestore(&mmu_lock, irq);
    return found;
}

int mmu_handle_cow(unsigned long* pgd, unsigned long vaddr)
{
    vaddr &= ~0xFFFUL;
    unsigned long paddr, flags;

    if (!mmu_user_query(pgd, vaddr, &paddr, &flags))
        return -1;
    if (!(flags & MMU_PTE_COW))
        return -1;

    void* new_page = pmm_alloc_page();
    if (!new_page)
        return -1;

    memcpy(new_page, (void*)P2V(paddr), PAGE_SIZE);

    unsigned long new_flags = flags & ~MMU_PTE_COW;
    new_flags &= ~(3ULL << 6);
    new_flags |= MMU_AP_RW;

    mmu_user_map_page(pgd, vaddr, V2P((uintptr_t)new_page), new_flags);

    return 0;
}
unsigned long mmu_kernel_ttbr0(void)
{
    return empty_pgd_phys;
}
