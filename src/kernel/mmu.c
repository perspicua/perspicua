#include "mmu.h"
#include "pmm.h"
#include "../lib/stdio.h"

// MAIR attribute indexes
#define MAIR_NORMAL_IDX 0
#define MAIR_DEVICE_IDX 1

// page table entry bits
#define PTE_VALID (1ULL << 0)
#define PTE_TABLE (1ULL << 1)
#define PTE_BLOCK (0ULL << 1)
#define PTE_AF (1ULL << 10)
#define PTE_SH_INNER (3ULL << 8)

#define PTE_ATTR_NORMAL ((unsigned long)MAIR_NORMAL_IDX << 2)
#define PTE_ATTR_DEVICE ((unsigned long)MAIR_DEVICE_IDX << 2)

// attr 0: normal write-back (0xFF), attr 1: device-nGnRnE (0x00)
#define MAIR_VALUE ((0xFFULL << (MAIR_NORMAL_IDX * 8)) | (0x00ULL << (MAIR_DEVICE_IDX * 8)))

void mmu_init(void)
{
    // allocate pages for level 1 (pgd) and two level 2 (pmd) tables
    unsigned long* pgd = (unsigned long*)pmm_alloc_page();
    unsigned long* pmd_ram = (unsigned long*)pmm_alloc_page();
    unsigned long* pmd_periph = (unsigned long*)pmm_alloc_page();

    for (int i = 0; i < 512; i++)
    {
        pgd[i] = 0;
        pmd_ram[i] = 0;
        pmd_periph[i] = 0;
    }

    // pgd[0] -> 0x00000000-0x3FFFFFFF (1GB RAM)
    pgd[0] = ((unsigned long)pmd_ram) | PTE_VALID | PTE_TABLE;
    // pgd[3] -> 0xC0000000-0xFFFFFFFF (1GB peripherals)
    pgd[3] = ((unsigned long)pmd_periph) | PTE_VALID | PTE_TABLE;

    // identity map RAM as 512 x 2MB blocks, cacheable
    for (unsigned long i = 0; i < 512; i++)
    {
        unsigned long addr = i * (2 * 1024 * 1024);
        pmd_ram[i] = addr | PTE_VALID | PTE_BLOCK | PTE_AF | PTE_SH_INNER | PTE_ATTR_NORMAL;
    }

    // identity map peripherals as 512 x 2MB blocks, device memory
    for (unsigned long i = 0; i < 512; i++)
    {
        unsigned long addr = 0xC0000000 + (i * 2 * 1024 * 1024);
        pmd_periph[i] = addr | PTE_VALID | PTE_BLOCK | PTE_AF | PTE_ATTR_DEVICE;
    }

    // set memory attributes
    asm volatile("msr mair_el1, %0" : : "r"(MAIR_VALUE));

    // configure TCR: 39-bit VA (T0SZ=25), 4KB granule, inner shareable, WB cacheable
    unsigned long tcr = (25ULL << 0) | (1ULL << 8) | (1ULL << 10) | (3ULL << 12);
    asm volatile("msr tcr_el1, %0" : : "r"(tcr));

    // point TTBR0 at our page table
    asm volatile("msr ttbr0_el1, %0" : : "r"((unsigned long)pgd));

    // invalidate TLB
    asm volatile("tlbi vmalle1is");
    asm volatile("dsb ish");
    asm volatile("isb");

    // enable MMU (M), data cache (C), instruction cache (I)
    unsigned long sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1 << 0) | (1 << 2) | (1 << 12);
    asm volatile("msr sctlr_el1, %0" : : "r"(sctlr));
    asm volatile("isb");

    printf("MMU: Enabled. Identity mapping and caches active.\n");
}