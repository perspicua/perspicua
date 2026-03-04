#include "mmu.h"
#include "pmm.h"
#include "../lib/stdio.h"

#define KERNEL_VMA 0xFFFFFF8000000000ULL
#define V2P(v) ((unsigned long)(v) - KERNEL_VMA)
#define P2V(p) ((unsigned long)(p) + KERNEL_VMA)

#define MAIR_NORMAL_IDX 0
#define MAIR_DEVICE_IDX 1

#define PTE_VALID (1ULL << 0)
#define PTE_TABLE (1ULL << 1)
#define PTE_PAGE (1ULL << 1)
#define PTE_BLOCK (0ULL << 1)
#define PTE_AF (1ULL << 10)
#define PTE_SH_INNER (3ULL << 8)

#define PTE_PXN (1ULL << 53)
#define PTE_UXN (1ULL << 54)
#define PTE_AP_RW (0ULL << 6)
#define PTE_AP_RO (2ULL << 6)

#define PTE_ATTR_NORMAL ((unsigned long)MAIR_NORMAL_IDX << 2)
#define PTE_ATTR_DEVICE ((unsigned long)MAIR_DEVICE_IDX << 2)

extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], __data_end[];
extern char __bss_start[], __bss_end[];

static unsigned long kernel_pgd_phys;

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

    kernel_pgd_phys = V2P(pgd);

    asm volatile("msr ttbr1_el1, %0" : : "r"(kernel_pgd_phys));
    asm volatile("msr ttbr0_el1, %0" : : "r"(0)); // Trap lower-half access

    asm volatile("tlbi vmalle1is");
    asm volatile("dsb ish");
    asm volatile("isb");

    printf("[  MMU ] TTBR1 → 0x%lx, TTBR0 nullified (trap user access)\n", kernel_pgd_phys);
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
    asm volatile("msr ttbr0_el1, %0" : : "r"(0));

    asm volatile("tlbi vmalle1is");
    asm volatile("dsb ish");
    asm volatile("isb");
}
