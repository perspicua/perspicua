/*
 * mmu.c - AArch64 Memory Management Unit driver.
 */

#include "mm/mmu.h"

#include <stddef.h>
#include <stdint.h>

#include "stdio.h"
#include "string.h"

#include "uapi/errno.h"

#include "core/lock.h"
#include "mm/addr.h"
#include "mm/asid.h"
#include "mm/pmm.h"
#include "panic.h"

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

#define L1_IDX(va) (((va) >> 30) & 0x1FFUL)
#define L2_IDX(va) (((va) >> 21) & 0x1FFUL)
#define L3_IDX(va) (((va) >> 12) & 0x1FFUL)
#define PG_OFF(va) ((va) & 0xFFFUL)

#define PT_ENTRIES    512UL
#define L2_BLOCK_SIZE (2UL * 1024 * 1024)
#define GB_SIZE       (1024UL * 1024 * 1024)

extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], __data_end[];
extern char __bss_start[], __bss_end[];
extern char __kernel_end[];
extern unsigned long pmm_metadata_end;

static unsigned long kernel_pgd_phys;
static unsigned long *kernel_pgd_virt;
static unsigned long empty_pgd_phys;

static spinlock_t mmu_lock = SPINLOCK_INIT;

static inline void tlbi_va_is(unsigned long vaddr)
{
    unsigned long va_op = (vaddr >> 12) & 0x00000FFFFFFFFFFFULL;
    asm volatile("dsb ishst" : : : "memory");
    asm volatile("tlbi vaae1is, %0" : : "r"(va_op));
    asm volatile("dsb ish" : : : "memory");
    asm volatile("isb" : : : "memory");
}

static inline void tlbi_all_is(void)
{
    asm volatile("dsb ishst" : : : "memory");
    asm volatile("tlbi vmalle1is" : : : "memory");
    asm volatile("dsb ish" : : : "memory");
    asm volatile("isb" : : : "memory");
}

// Table allocation for early boot initialization; panics if memory is exhausted.
static unsigned long *boot_table_page(void)
{
    unsigned long *page = (unsigned long *)pmm_alloc_page();
    if (!page) {
        PANIC("mmu: boot table allocation failed");
    }
    return page;
}

// Runtime table allocation; returns NULL on allocation failure.
static unsigned long *alloc_table_page(void)
{
    return (unsigned long *)pmm_alloc_page();
}

static int overlaps_ram(unsigned long start, unsigned long end, const struct pmm_range *ram,
                        int ram_count)
{
    for (int i = 0; i < ram_count; i++) {
        if (start < ram[i].end && ram[i].start < end) {
            return 1;
        }
    }
    return 0;
}

// Linear-map descriptor bits for pa, without the block/page type bit.
static unsigned long linear_attr(unsigned long pa)
{
    unsigned long attr = PTE_VALID | PTE_AF | PTE_PXN | PTE_UXN | PTE_AP_RW;

    if (pa >= BCM2711_PERIPH_BASE && pa < 0x100000000UL) {
        return attr | PTE_ATTR_D;
    }
    return attr | PTE_ATTR_N | PTE_SH_INNER;
}

// RAM gets 4 KB pages, so no block with RAM behind it is ever split under live cores.
static void map_linear_gb(unsigned long *l2t, unsigned long gb_base, const struct pmm_range *ram,
                          int ram_count)
{
    for (unsigned long i = 0; i < PT_ENTRIES; i++) {
        unsigned long base = gb_base + i * L2_BLOCK_SIZE;
        if (!overlaps_ram(base, base + L2_BLOCK_SIZE, ram, ram_count)) {
            l2t[i] = base | PTE_BLOCK | linear_attr(base);
            continue;
        }

        unsigned long *l3 = boot_table_page();
        for (unsigned long j = 0; j < PT_ENTRIES; j++) {
            unsigned long pa = base + j * PAGE_SIZE;
            l3[j] = pa | PTE_PAGE | linear_attr(pa);
        }
        l2t[i] = V2P((uintptr_t)l3) | PTE_VALID | PTE_TABLE;
    }
}

// Every core touching the block faults during the break, so it must have no RAM behind it.
static unsigned long *split_block_to_pages(unsigned long *l2_table, unsigned long l2_idx,
                                           unsigned long vaddr_base)
{
    unsigned long block_entry = l2_table[l2_idx];
    unsigned long block_phys = block_entry & PTE_ADDR;
    unsigned long block_attr = block_entry & ~PTE_ADDR & ~PTE_TABLE;

    const struct pmm_range *ram;
    int ram_count = pmm_get_ram_ranges(&ram);
    if (overlaps_ram(block_phys, block_phys + L2_BLOCK_SIZE, ram, ram_count)) {
        PANIC("mmu: refusing to split a 2 MB block with RAM behind it");
    }

    unsigned long *l3 = alloc_table_page();
    if (!l3) {
        return NULL;
    }

    for (unsigned long i = 0; i < PT_ENTRIES; i++) {
        unsigned long page_phys = block_phys + i * PAGE_SIZE;
        l3[i] = page_phys | PTE_VALID | PTE_PAGE | block_attr;
    }

    l2_table[l2_idx] = 0;
    asm volatile("dsb ishst" : : : "memory");

    for (unsigned long v = vaddr_base; v < vaddr_base + L2_BLOCK_SIZE; v += PAGE_SIZE) {
        tlbi_va_is(v);
    }

    l2_table[l2_idx] = V2P((uintptr_t)l3) | PTE_VALID | PTE_TABLE;
    asm volatile("dsb ish" : : : "memory");
    asm volatile("isb" : : : "memory");

    return l3;
}

void mmu_init(void)
{
    unsigned long *pgd = boot_table_page();
    unsigned long *pmd_0 = boot_table_page();
    unsigned long *pmd_1 = boot_table_page();
    unsigned long *pmd_2 = boot_table_page();
    unsigned long *pmd_3 = boot_table_page();

    pgd[0] = V2P((uintptr_t)pmd_0) | PTE_VALID | PTE_TABLE;
    pgd[1] = V2P((uintptr_t)pmd_1) | PTE_VALID | PTE_TABLE;
    pgd[2] = V2P((uintptr_t)pmd_2) | PTE_VALID | PTE_TABLE;
    pgd[3] = V2P((uintptr_t)pmd_3) | PTE_VALID | PTE_TABLE;

    // The first GB is all 4 KB pages, for per-page kernel and framebuffer attributes.
    const struct pmm_range first_gb = {0, GB_SIZE};
    map_linear_gb(pmd_0, 0, &first_gb, 1);

    const struct pmm_range *ram;
    int ram_count = pmm_get_ram_ranges(&ram);
    unsigned long *pmd_others[3] = {pmd_1, pmd_2, pmd_3};
    for (unsigned long p = 0; p < 3; p++) {
        map_linear_gb(pmd_others[p], (p + 1) * GB_SIZE, ram, ram_count);
    }

    kernel_pgd_virt = pgd;
    kernel_pgd_phys = V2P((uintptr_t)pgd);

    unsigned long ks = (unsigned long)__text_start;
    unsigned long ke = (unsigned long)pmm_metadata_end;

    for (unsigned long va = ks; va < ke; va += PAGE_SIZE) {
        unsigned long pa = V2P(va);
        unsigned long attr = PTE_VALID | PTE_PAGE | PTE_AF | PTE_SH_INNER | PTE_ATTR_N;

        if (va >= (unsigned long)__text_start && va < (unsigned long)__text_end) {
            attr |= PTE_AP_RO | PTE_UXN;
        } else if (va >= (unsigned long)__rodata_start && va < (unsigned long)__rodata_end) {
            attr |= PTE_AP_RO | PTE_PXN | PTE_UXN;
        } else {
            attr |= PTE_AP_RW | PTE_PXN | PTE_UXN;
        }

        if (mmu_map_page(va, pa, attr) != 0) {
            PANIC("mmu: failed to map kernel segment page");
        }
    }

    unsigned long *empty_pgd = boot_table_page();
    empty_pgd_phys = V2P((uintptr_t)empty_pgd);

    asm volatile("msr ttbr1_el1, %0" : : "r"(kernel_pgd_phys) : "memory");
    asm volatile("msr ttbr0_el1, %0" : : "r"(empty_pgd_phys) : "memory");
    tlbi_all_is();
    asid_init();

    pr_info("mmu: TTBR1 -> 0x%lx, TTBR0 -> empty (0x%lx)\n", kernel_pgd_phys, empty_pgd_phys);
}

void mmu_secondary_init(void)
{
    asm volatile("msr ttbr1_el1, %0" : : "r"(kernel_pgd_phys) : "memory");
    asm volatile("msr ttbr0_el1, %0" : : "r"(empty_pgd_phys) : "memory");
    tlbi_all_is();
}

int mmu_map_page(unsigned long vaddr, unsigned long paddr, unsigned long flags)
{
    paddr &= PTE_ADDR;
    unsigned long irq = spin_lock_irqsave(&mmu_lock);

    unsigned long l1 = L1_IDX(vaddr);
    unsigned long l2 = L2_IDX(vaddr);
    unsigned long l3 = L3_IDX(vaddr);

    int new_l2 = 0;
    unsigned long *l2t;
    if (kernel_pgd_virt[l1] & PTE_VALID) {
        l2t = (unsigned long *)P2V(kernel_pgd_virt[l1] & PTE_ADDR);
    } else {
        l2t = alloc_table_page();
        if (!l2t) {
            spin_unlock_irqrestore(&mmu_lock, irq);
            return -ENOMEM;
        }
        new_l2 = 1;
    }

    unsigned long *l3t;
    unsigned long l2e = l2t[l2];

    if (l2e & PTE_VALID) {
        if (!(l2e & PTE_TABLE)) {
            unsigned long base = vaddr & ~(L2_BLOCK_SIZE - 1);
            l3t = split_block_to_pages(l2t, l2, base);
            if (!l3t) {
                spin_unlock_irqrestore(&mmu_lock, irq);
                return -ENOMEM;
            }
        } else {
            l3t = (unsigned long *)P2V(l2e & PTE_ADDR);
        }
    } else {
        l3t = alloc_table_page();
        if (!l3t) {
            if (new_l2) {
                pmm_free_page(l2t);
            }
            spin_unlock_irqrestore(&mmu_lock, irq);
            return -ENOMEM;
        }
        l2t[l2] = V2P((uintptr_t)l3t) | PTE_VALID | PTE_TABLE;
    }

    if (new_l2) {
        kernel_pgd_virt[l1] = V2P((uintptr_t)l2t) | PTE_VALID | PTE_TABLE;
    }

    if (l3t[l3] & PTE_VALID) {
        l3t[l3] = 0;
        tlbi_va_is(vaddr);
    }

    l3t[l3] = paddr | PTE_VALID | PTE_PAGE | flags;
    tlbi_va_is(vaddr);

    spin_unlock_irqrestore(&mmu_lock, irq);
    return 0;
}

int mmu_unmap_page(unsigned long vaddr)
{
    unsigned long irq = spin_lock_irqsave(&mmu_lock);

    unsigned long l1 = L1_IDX(vaddr);
    unsigned long l2 = L2_IDX(vaddr);
    unsigned long l3 = L3_IDX(vaddr);

    if (!(kernel_pgd_virt[l1] & PTE_VALID)) {
        spin_unlock_irqrestore(&mmu_lock, irq);
        return 0;
    }

    unsigned long *l2t = (unsigned long *)P2V(kernel_pgd_virt[l1] & PTE_ADDR);
    unsigned long l2e = l2t[l2];

    if (!(l2e & PTE_VALID)) {
        spin_unlock_irqrestore(&mmu_lock, irq);
        return 0;
    }

    unsigned long *l3t;
    if (!(l2e & PTE_TABLE)) {
        unsigned long base = vaddr & ~(L2_BLOCK_SIZE - 1);
        l3t = split_block_to_pages(l2t, l2, base);
        if (!l3t) {
            spin_unlock_irqrestore(&mmu_lock, irq);
            return -ENOMEM;
        }
    } else {
        l3t = (unsigned long *)P2V(l2e & PTE_ADDR);
    }

    if (l3t[l3] & PTE_VALID) {
        l3t[l3] = 0;
        tlbi_va_is(vaddr);
    }

    spin_unlock_irqrestore(&mmu_lock, irq);
    return 0;
}

int mmu_query(unsigned long vaddr, unsigned long *out_paddr, unsigned long *out_flags)
{
    unsigned long irq = spin_lock_irqsave(&mmu_lock);
    int found = 0;

    unsigned long l1 = L1_IDX(vaddr);
    unsigned long l2 = L2_IDX(vaddr);
    unsigned long l3 = L3_IDX(vaddr);

    if (!(kernel_pgd_virt[l1] & PTE_VALID)) {
        goto out;
    }

    unsigned long *l2t = (unsigned long *)P2V(kernel_pgd_virt[l1] & PTE_ADDR);
    unsigned long l2e = l2t[l2];

    if (!(l2e & PTE_VALID)) {
        goto out;
    }

    if (!(l2e & PTE_TABLE)) {
        if (out_paddr) {
            *out_paddr = (l2e & PTE_ADDR) + (vaddr & 0x1FFFFFUL);
        }
        if (out_flags) {
            *out_flags = l2e & ~PTE_ADDR;
        }
        found = 1;
        goto out;
    }

    unsigned long *l3t = (unsigned long *)P2V(l2e & PTE_ADDR);
    unsigned long l3e = l3t[l3];

    if (!(l3e & PTE_VALID)) {
        goto out;
    }

    if (out_paddr) {
        *out_paddr = (l3e & PTE_ADDR) + PG_OFF(vaddr);
    }
    if (out_flags) {
        *out_flags = l3e & ~PTE_ADDR;
    }
    found = 1;

out:
    spin_unlock_irqrestore(&mmu_lock, irq);
    return found;
}

unsigned long *mmu_create_user_pgd(void)
{
    return alloc_table_page();
}

// No mmu_lock: the table must already be private, out of the process table and off TTBR0.
void mmu_destroy_user_pgd(unsigned long *pgd)
{
    if (!pgd) {
        return;
    }

    unsigned long live_ttbr0;
    asm volatile("mrs %0, ttbr0_el1" : "=r"(live_ttbr0));
    if ((live_ttbr0 & PTE_ADDR) == V2P((uintptr_t)pgd)) {
        PANIC("mmu: destroying the address space still installed in TTBR0");
    }

    for (unsigned long i = 0; i < PT_ENTRIES; i++) {
        unsigned long l1e = pgd[i];
        if (!(l1e & PTE_VALID) || !(l1e & PTE_TABLE)) {
            continue;
        }

        unsigned long *l2 = (unsigned long *)P2V(l1e & PTE_ADDR);

        for (unsigned long j = 0; j < PT_ENTRIES; j++) {
            unsigned long l2e = l2[j];
            if (!(l2e & PTE_VALID)) {
                continue;
            }

            if (!(l2e & PTE_TABLE)) {
                void *vaddr = (void *)P2V(l2e & PTE_ADDR);
                if (pmm_is_managed(vaddr)) {
                    pmm_free_page(vaddr);
                }
                continue;
            }

            unsigned long *l3 = (unsigned long *)P2V(l2e & PTE_ADDR);
            for (unsigned long k = 0; k < PT_ENTRIES; k++) {
                unsigned long l3e = l3[k];
                if ((l3e & PTE_VALID) && (l3e & PTE_PAGE)) {
                    void *vaddr = (void *)P2V(l3e & PTE_ADDR);
                    if (pmm_is_managed(vaddr)) {
                        pmm_free_page(vaddr);
                    }
                }
            }
            pmm_free_page(l3);
        }
        pmm_free_page(l2);
    }
    pmm_free_page(pgd);
}

unsigned long *mmu_copy_user_pgd(unsigned long *parent_pgd)
{
    if (!parent_pgd) {
        return NULL;
    }

    unsigned long *child_pgd = mmu_create_user_pgd();
    if (!child_pgd) {
        return NULL;
    }

    unsigned long irq = spin_lock_irqsave(&mmu_lock);

    for (unsigned long i = 0; i < PT_ENTRIES; i++) {
        unsigned long l1e = parent_pgd[i];
        if (!(l1e & MMU_PTE_VALID)) {
            continue;
        }

        unsigned long *parent_pmd = (unsigned long *)P2V(l1e & PTE_ADDR);
        unsigned long *child_pmd = alloc_table_page();
        if (!child_pmd) {
            goto oom;
        }

        child_pgd[i] = V2P((uintptr_t)child_pmd) | MMU_PTE_VALID | MMU_PTE_TABLE;

        for (unsigned long j = 0; j < PT_ENTRIES; j++) {
            unsigned long l2e = parent_pmd[j];
            if (!(l2e & MMU_PTE_VALID)) {
                continue;
            }

            if (!(l2e & MMU_PTE_TABLE)) {
                unsigned long pte = l2e;
                if (!(pte & MMU_AP_RO)) {
                    pte = (pte & ~MMU_AP_RO) | MMU_AP_RO | MMU_PTE_COW;
                }
                child_pmd[j] = pte;
                void *v_pa = (void *)P2V(pte & PTE_ADDR);
                if (pmm_is_managed(v_pa)) {
                    pmm_hold_page(v_pa);
                }
                continue;
            }

            unsigned long *parent_pte = (unsigned long *)P2V(l2e & PTE_ADDR);
            unsigned long *child_pte = alloc_table_page();
            if (!child_pte) {
                goto oom;
            }

            child_pmd[j] = V2P((uintptr_t)child_pte) | MMU_PTE_VALID | MMU_PTE_TABLE;

            for (unsigned long k = 0; k < PT_ENTRIES; k++) {
                unsigned long pte = parent_pte[k];
                if (!(pte & MMU_PTE_VALID)) {
                    continue;
                }

                if (!(pte & MMU_AP_RO)) {
                    pte = (pte & ~MMU_AP_RO) | MMU_AP_RO | MMU_PTE_COW;
                }
                child_pte[k] = pte;
                void *v_pa = (void *)P2V(pte & PTE_ADDR);
                if (pmm_is_managed(v_pa)) {
                    pmm_hold_page(v_pa);
                }
            }
        }
    }

    // Mark parent writable pages as copy-on-write once all child tables exist
    for (unsigned long i = 0; i < PT_ENTRIES; i++) {
        unsigned long l1e = parent_pgd[i];
        if (!(l1e & MMU_PTE_VALID)) {
            continue;
        }

        unsigned long *parent_pmd = (unsigned long *)P2V(l1e & PTE_ADDR);
        for (unsigned long j = 0; j < PT_ENTRIES; j++) {
            unsigned long l2e = parent_pmd[j];
            if (!(l2e & MMU_PTE_VALID)) {
                continue;
            }

            if (!(l2e & MMU_PTE_TABLE)) {
                if (!(l2e & MMU_AP_RO)) {
                    parent_pmd[j] = (l2e & ~MMU_AP_RO) | MMU_AP_RO | MMU_PTE_COW;
                }
                continue;
            }

            unsigned long *parent_pte = (unsigned long *)P2V(l2e & PTE_ADDR);
            for (unsigned long k = 0; k < PT_ENTRIES; k++) {
                unsigned long pte = parent_pte[k];
                if ((pte & MMU_PTE_VALID) && !(pte & MMU_AP_RO)) {
                    parent_pte[k] = (pte & ~MMU_AP_RO) | MMU_AP_RO | MMU_PTE_COW;
                }
            }
        }
    }

    tlbi_all_is();
    spin_unlock_irqrestore(&mmu_lock, irq);
    return child_pgd;

oom:
    spin_unlock_irqrestore(&mmu_lock, irq);
    mmu_destroy_user_pgd(child_pgd);
    return NULL;
}

int mmu_user_map_page(unsigned long *pgd, unsigned long vaddr, unsigned long paddr,
                      unsigned long flags)
{
    if (!pgd) {
        PANIC("mmu: NULL user pgd");
    }

    paddr &= PTE_ADDR;
    unsigned long irq = spin_lock_irqsave(&mmu_lock);

    unsigned long l1 = L1_IDX(vaddr);
    unsigned long l2 = L2_IDX(vaddr);
    unsigned long l3 = L3_IDX(vaddr);

    int new_l2 = 0;
    unsigned long *l2t;
    if (pgd[l1] & PTE_VALID) {
        l2t = (unsigned long *)P2V(pgd[l1] & PTE_ADDR);
    } else {
        l2t = alloc_table_page();
        if (!l2t) {
            spin_unlock_irqrestore(&mmu_lock, irq);
            return -ENOMEM;
        }
        new_l2 = 1;
    }

    unsigned long *l3t;
    if (l2t[l2] & PTE_VALID) {
        if (!(l2t[l2] & PTE_TABLE)) {
            unsigned long base = vaddr & ~(L2_BLOCK_SIZE - 1);
            l3t = split_block_to_pages(l2t, l2, base);
            if (!l3t) {
                spin_unlock_irqrestore(&mmu_lock, irq);
                return -ENOMEM;
            }
        } else {
            l3t = (unsigned long *)P2V(l2t[l2] & PTE_ADDR);
        }
    } else {
        l3t = alloc_table_page();
        if (!l3t) {
            if (new_l2) {
                pmm_free_page(l2t);
            }
            spin_unlock_irqrestore(&mmu_lock, irq);
            return -ENOMEM;
        }
        l2t[l2] = V2P((uintptr_t)l3t) | PTE_VALID | PTE_TABLE;
    }

    if (new_l2) {
        pgd[l1] = V2P((uintptr_t)l2t) | PTE_VALID | PTE_TABLE;
    }

    if (l3t[l3] & PTE_VALID) {
        unsigned long old_pa = l3t[l3] & PTE_ADDR;
        l3t[l3] = 0;
        tlbi_va_is(vaddr);
        void *v_old_pa = (void *)P2V(old_pa);
        if (pmm_is_managed(v_old_pa)) {
            pmm_free_page(v_old_pa);
        }
    }

    l3t[l3] = paddr | PTE_VALID | PTE_PAGE | flags;
    tlbi_va_is(vaddr);

    spin_unlock_irqrestore(&mmu_lock, irq);
    return 0;
}

int mmu_user_unmap_page(unsigned long *pgd, unsigned long vaddr)
{
    if (!pgd) {
        return 0;
    }

    unsigned long irq = spin_lock_irqsave(&mmu_lock);

    unsigned long l1 = L1_IDX(vaddr);
    unsigned long l2 = L2_IDX(vaddr);
    unsigned long l3 = L3_IDX(vaddr);

    if (!(pgd[l1] & PTE_VALID)) {
        spin_unlock_irqrestore(&mmu_lock, irq);
        return 0;
    }

    unsigned long *l2t = (unsigned long *)P2V(pgd[l1] & PTE_ADDR);
    unsigned long l2e = l2t[l2];

    if (!(l2e & PTE_VALID)) {
        spin_unlock_irqrestore(&mmu_lock, irq);
        return 0;
    }

    unsigned long *l3t;
    if (!(l2e & PTE_TABLE)) {
        unsigned long base = vaddr & ~(L2_BLOCK_SIZE - 1);
        l3t = split_block_to_pages(l2t, l2, base);
        if (!l3t) {
            spin_unlock_irqrestore(&mmu_lock, irq);
            return -ENOMEM;
        }
    } else {
        l3t = (unsigned long *)P2V(l2e & PTE_ADDR);
    }

    if (l3t[l3] & PTE_VALID) {
        unsigned long old_pa = l3t[l3] & PTE_ADDR;
        l3t[l3] = 0;
        tlbi_va_is(vaddr);
        void *v_old_pa = (void *)P2V(old_pa);
        if (pmm_is_managed(v_old_pa)) {
            pmm_free_page(v_old_pa);
        }
    }

    spin_unlock_irqrestore(&mmu_lock, irq);
    return 0;
}

void mmu_switch_user(unsigned long *pgd, unsigned long asid)
{
    unsigned long ttbr0 = V2P((uintptr_t)pgd) | asid_ttbr_field(asid);

    asm volatile("msr ttbr0_el1, %0" : : "r"(ttbr0) : "memory");
    asm volatile("dsb ish" : : : "memory");
    asm volatile("isb" : : : "memory");
}

// A copy-on-write page reads as read-only but counts as writable: its first store makes it so.
static int pte_user_ok(unsigned long pte, int writable)
{
    if (!(pte & MMU_AP_USER)) {
        return 0;
    }
    if (writable && (pte & MMU_AP_RO) && !(pte & MMU_PTE_COW)) {
        return 0;
    }
    return 1;
}

// One descent per table and one mmu_lock hold for the whole range, not one per page.
int mmu_user_range_ok(unsigned long *pgd, unsigned long start, unsigned long end, int writable)
{
    if (!pgd || end <= start) {
        return 0;
    }

    unsigned long irq = spin_lock_irqsave(&mmu_lock);
    unsigned long va = start & ~0xFFFUL;
    int ok = 1;

    while (va < end && ok) {
        unsigned long l1e = pgd[L1_IDX(va)];
        if (!(l1e & PTE_VALID)) {
            ok = 0;
            break;
        }

        unsigned long *l2t = (unsigned long *)P2V(l1e & PTE_ADDR);
        unsigned long l2e = l2t[L2_IDX(va)];
        if (!(l2e & PTE_VALID)) {
            ok = 0;
            break;
        }

        // A 2 MB block is one permission for the whole span.
        if (!(l2e & PTE_TABLE)) {
            if (!pte_user_ok(l2e, writable)) {
                ok = 0;
                break;
            }
            va = (va & ~0x1FFFFFUL) + 0x200000UL;
            continue;
        }

        unsigned long *l3t = (unsigned long *)P2V(l2e & PTE_ADDR);
        for (unsigned long i = L3_IDX(va); i < PT_ENTRIES && va < end; i++, va += PAGE_SIZE) {
            unsigned long l3e = l3t[i];
            if (!(l3e & PTE_VALID) || !pte_user_ok(l3e, writable)) {
                ok = 0;
                break;
            }
        }
    }

    spin_unlock_irqrestore(&mmu_lock, irq);
    return ok;
}

// Before a pgd is freed: the table walker may prefetch through TTBR0 even with nothing in the TLB.
void mmu_leave_user(void)
{
    asm volatile("msr ttbr0_el1, %0" : : "r"(empty_pgd_phys) : "memory");
    asm volatile("dsb ish" : : : "memory");
    asm volatile("isb" : : : "memory");
}

int mmu_user_query(unsigned long *pgd, unsigned long vaddr, unsigned long *out_paddr,
                   unsigned long *out_flags)
{
    if (!pgd) {
        return 0;
    }

    unsigned long irq = spin_lock_irqsave(&mmu_lock);
    int found = 0;

    unsigned long l1 = L1_IDX(vaddr);
    unsigned long l2 = L2_IDX(vaddr);
    unsigned long l3 = L3_IDX(vaddr);

    if (!(pgd[l1] & PTE_VALID)) {
        goto out;
    }

    unsigned long *l2t = (unsigned long *)P2V(pgd[l1] & PTE_ADDR);
    unsigned long l2e = l2t[l2];

    if (!(l2e & PTE_VALID)) {
        goto out;
    }

    if (!(l2e & PTE_TABLE)) {
        if (out_paddr) {
            *out_paddr = (l2e & PTE_ADDR) + (vaddr & 0x1FFFFFUL);
        }
        if (out_flags) {
            *out_flags = l2e & ~PTE_ADDR;
        }
        found = 1;
        goto out;
    }

    unsigned long *l3t = (unsigned long *)P2V(l2e & PTE_ADDR);
    unsigned long l3e = l3t[l3];

    if (!(l3e & PTE_VALID)) {
        goto out;
    }

    if (out_paddr) {
        *out_paddr = (l3e & PTE_ADDR) + PG_OFF(vaddr);
    }
    if (out_flags) {
        *out_flags = l3e & ~PTE_ADDR;
    }
    found = 1;

out:
    spin_unlock_irqrestore(&mmu_lock, irq);
    return found;
}

// Caller holds mmu_lock. NULL for an absent level or a 2 MB block, neither of which can be CoW.
static unsigned long *user_leaf_pte(unsigned long *pgd, unsigned long vaddr)
{
    if (!pgd || !(pgd[L1_IDX(vaddr)] & PTE_VALID)) {
        return NULL;
    }

    unsigned long *l2t = (unsigned long *)P2V(pgd[L1_IDX(vaddr)] & PTE_ADDR);
    unsigned long l2e = l2t[L2_IDX(vaddr)];
    if (!(l2e & PTE_VALID) || !(l2e & PTE_TABLE)) {
        return NULL;
    }

    unsigned long *l3t = (unsigned long *)P2V(l2e & PTE_ADDR);
    return &l3t[L3_IDX(vaddr)];
}

// Copies only a page still shared; the copy runs unlocked and is discarded if the entry moved.
int mmu_handle_cow(unsigned long *pgd, unsigned long vaddr)
{
    vaddr &= ~0xFFFUL;

    unsigned long irq = spin_lock_irqsave(&mmu_lock);
    unsigned long *pte = user_leaf_pte(pgd, vaddr);
    if (!pte || !(*pte & PTE_VALID) || !(*pte & MMU_PTE_COW)) {
        spin_unlock_irqrestore(&mmu_lock, irq);
        return -1;
    }

    unsigned long old_pa = *pte & PTE_ADDR;
    void *old_va = (void *)P2V(old_pa);

    // Sole owner: promote in place, no allocation and no copy.
    if (!pmm_is_managed(old_va) || pmm_page_refcount(old_va) <= 1) {
        *pte = (*pte & ~MMU_PTE_COW & ~MMU_AP_RO) | MMU_AP_RW;
        tlbi_va_is(vaddr);
        spin_unlock_irqrestore(&mmu_lock, irq);
        return 0;
    }

    pmm_hold_page(old_va); // keep the source alive while the lock is dropped
    spin_unlock_irqrestore(&mmu_lock, irq);

    void *new_page = pmm_alloc_pages_nozero(1);
    if (!new_page) {
        pmm_free_page(old_va);
        return -ENOMEM;
    }
    memcpy(new_page, old_va, PAGE_SIZE);

    irq = spin_lock_irqsave(&mmu_lock);
    pte = user_leaf_pte(pgd, vaddr);
    if (!pte || !(*pte & PTE_VALID) || !(*pte & MMU_PTE_COW) || (*pte & PTE_ADDR) != old_pa) {
        // Resolved or remapped while we copied; let the faulting access retry.
        spin_unlock_irqrestore(&mmu_lock, irq);
        pmm_free_page(new_page);
        pmm_free_page(old_va);
        return 0;
    }

    // A new output address on a live entry needs break-before-make.
    unsigned long new_flags = (*pte & ~PTE_ADDR & ~MMU_PTE_COW & ~MMU_AP_RO) | MMU_AP_RW;
    *pte = 0;
    tlbi_va_is(vaddr);
    *pte = V2P((uintptr_t)new_page) | new_flags;
    tlbi_va_is(vaddr);
    spin_unlock_irqrestore(&mmu_lock, irq);

    pmm_free_page(old_va); // our reference
    pmm_free_page(old_va); // the mapping's, now replaced
    return 0;
}

unsigned long mmu_kernel_ttbr0(void)
{
    return empty_pgd_phys;
}

#ifdef CONFIG_TESTS
void mmu_test_map_linear_gb(unsigned long *l2t, unsigned long gb_base, const struct pmm_range *ram,
                            int ram_count)
{
    map_linear_gb(l2t, gb_base, ram, ram_count);
}

int mmu_test_is_block_entry(unsigned long vaddr)
{
    unsigned long irq = spin_lock_irqsave(&mmu_lock);

    unsigned long l1 = L1_IDX(vaddr);
    unsigned long l2 = L2_IDX(vaddr);
    int result = 0;

    if (!(kernel_pgd_virt[l1] & PTE_VALID)) {
        goto out;
    }
    unsigned long *l2t = (unsigned long *)P2V(kernel_pgd_virt[l1] & PTE_ADDR);
    unsigned long l2e = l2t[l2];
    if (!(l2e & PTE_VALID)) {
        goto out;
    }
    result = !(l2e & PTE_TABLE);

out:
    spin_unlock_irqrestore(&mmu_lock, irq);
    return result;
}
#endif
