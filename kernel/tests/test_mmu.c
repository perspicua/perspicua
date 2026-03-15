#include "test.h"
#include "mmu.h"
#include "pmm.h"
#include "addr.h"
#include "string.h"

#define TEST_VA_BASE (KERNEL_VMA + 0x100000000ULL)  // PGD index 4

void test_mmu(void)
{
    TEST_SUITE_BEGIN("MMU Dynamic Mapping");

    // basic map and query
    {
        void* phys_page     = pmm_alloc_page();
        unsigned long paddr = V2P(phys_page);
        unsigned long vaddr = TEST_VA_BASE;

        mmu_map_page(vaddr, paddr, MMU_FLAGS_KERNEL_RW);

        unsigned long out_paddr = 0;
        unsigned long out_flags = 0;
        int mapped              = mmu_query(vaddr, &out_paddr, &out_flags);
        TEST_ASSERT("map: query returns mapped", mapped == 1);
        TEST_ASSERT("map: paddr correct", out_paddr == paddr);
        TEST_ASSERT("map: valid+page bits set",
                    (out_flags & (MMU_PTE_VALID | MMU_PTE_PAGE)) == (MMU_PTE_VALID | MMU_PTE_PAGE));

        mmu_unmap_page(vaddr);
        pmm_free_page(phys_page);
    }
    TEST_PASS("basic map+query");

    // unmap makes page not-mapped
    {
        void* phys_page     = pmm_alloc_page();
        unsigned long paddr = V2P(phys_page);
        unsigned long vaddr = TEST_VA_BASE + 0x1000;  // next page

        mmu_map_page(vaddr, paddr, MMU_FLAGS_KERNEL_RW);
        mmu_unmap_page(vaddr);

        int mapped = mmu_query(vaddr, 0, 0);
        TEST_ASSERT("unmap: query returns unmapped", mapped == 0);

        pmm_free_page(phys_page);
    }
    TEST_PASS("unmap");

    // query unmapped address returns 0
    {
        // PGD index 5, never mapped
        unsigned long vaddr = KERNEL_VMA + 0x140000000ULL;
        int mapped          = mmu_query(vaddr, 0, 0);
        TEST_ASSERT("query unmapped returns 0", mapped == 0);
    }
    TEST_PASS("query unmapped");

    // map, write, read through new mapping
    {
        void* phys_page     = pmm_alloc_page();
        unsigned long paddr = V2P(phys_page);
        unsigned long vaddr = TEST_VA_BASE + 0x2000;

        mmu_map_page(vaddr, paddr, MMU_FLAGS_KERNEL_RW);

        // write through virtual address
        volatile unsigned long* ptr = (volatile unsigned long*)vaddr;
        *ptr                        = 0xCAFEBABEDEADBEEFULL;

        // read through the original kernel-mapped VA (P2V of the same phys page)
        volatile unsigned long* direct = (volatile unsigned long*)phys_page;
        TEST_ASSERT("write through mapping", *direct == 0xCAFEBABEDEADBEEFULL);

        mmu_unmap_page(vaddr);
        pmm_free_page(phys_page);
    }
    TEST_PASS("write through mapping");

    // map multiple pages in same L2 region
    {
        void* p1         = pmm_alloc_page();
        void* p2         = pmm_alloc_page();
        void* p3         = pmm_alloc_page();
        unsigned long v1 = TEST_VA_BASE + 0x3000;
        unsigned long v2 = TEST_VA_BASE + 0x4000;
        unsigned long v3 = TEST_VA_BASE + 0x5000;

        mmu_map_page(v1, V2P(p1), MMU_FLAGS_KERNEL_RW);
        mmu_map_page(v2, V2P(p2), MMU_FLAGS_KERNEL_RW);
        mmu_map_page(v3, V2P(p3), MMU_FLAGS_KERNEL_RW);

        // write distinct values
        *(volatile unsigned long*)v1 = 0x1111;
        *(volatile unsigned long*)v2 = 0x2222;
        *(volatile unsigned long*)v3 = 0x3333;

        TEST_ASSERT("multi-page: p1 value", *(volatile unsigned long*)p1 == 0x1111);
        TEST_ASSERT("multi-page: p2 value", *(volatile unsigned long*)p2 == 0x2222);
        TEST_ASSERT("multi-page: p3 value", *(volatile unsigned long*)p3 == 0x3333);

        // verify queries
        unsigned long pa;
        TEST_ASSERT("multi-page: q1", mmu_query(v1, &pa, 0) && pa == V2P(p1));
        TEST_ASSERT("multi-page: q2", mmu_query(v2, &pa, 0) && pa == V2P(p2));
        TEST_ASSERT("multi-page: q3", mmu_query(v3, &pa, 0) && pa == V2P(p3));

        mmu_unmap_page(v1);
        mmu_unmap_page(v2);
        mmu_unmap_page(v3);
        pmm_free_page(p1);
        pmm_free_page(p2);
        pmm_free_page(p3);
    }
    TEST_PASS("multiple pages same L2");

    // map pages across different L2 entries (different 2MB regions)
    {
        void* p1 = pmm_alloc_page();
        void* p2 = pmm_alloc_page();
        // v1 in first 2MB of PGD[4], v2 in second 2MB
        unsigned long v1 = TEST_VA_BASE + 0x6000;
        unsigned long v2 = TEST_VA_BASE + 0x200000 + 0x6000;  // +2MB

        mmu_map_page(v1, V2P(p1), MMU_FLAGS_KERNEL_RW);
        mmu_map_page(v2, V2P(p2), MMU_FLAGS_KERNEL_RW);

        *(volatile unsigned long*)v1 = 0xAAAA;
        *(volatile unsigned long*)v2 = 0xBBBB;

        TEST_ASSERT("cross-L2: p1", *(volatile unsigned long*)p1 == 0xAAAA);
        TEST_ASSERT("cross-L2: p2", *(volatile unsigned long*)p2 == 0xBBBB);

        mmu_unmap_page(v1);
        mmu_unmap_page(v2);
        pmm_free_page(p1);
        pmm_free_page(p2);
    }
    TEST_PASS("pages across L2 entries");

    // query existing 2MB block mapping (set up by mmu_init)
    {
        // The kernel maps physical 0x200000 (2MB) as a 2MB block in PGD[0]/PMD[1]
        unsigned long vaddr = KERNEL_VMA + 0x200000;
        unsigned long pa;
        int mapped = mmu_query(vaddr, &pa, 0);
        TEST_ASSERT("2MB block: mapped", mapped == 1);
        TEST_ASSERT("2MB block: paddr", pa == 0x200000);
    }
    TEST_PASS("query 2MB block mapping");

    // map and unmap cycle — re-map same VA to different phys
    {
        void* p1            = pmm_alloc_page();
        void* p2            = pmm_alloc_page();
        unsigned long vaddr = TEST_VA_BASE + 0x7000;

        mmu_map_page(vaddr, V2P(p1), MMU_FLAGS_KERNEL_RW);
        *(volatile unsigned long*)vaddr = 0x1234;
        TEST_ASSERT("remap: first mapping", *(volatile unsigned long*)p1 == 0x1234);
        mmu_unmap_page(vaddr);

        mmu_map_page(vaddr, V2P(p2), MMU_FLAGS_KERNEL_RW);
        *(volatile unsigned long*)vaddr = 0x5678;
        TEST_ASSERT("remap: second mapping", *(volatile unsigned long*)p2 == 0x5678);
        // p1 should still have old value
        TEST_ASSERT("remap: p1 unchanged", *(volatile unsigned long*)p1 == 0x1234);
        mmu_unmap_page(vaddr);

        pmm_free_page(p1);
        pmm_free_page(p2);
    }
    TEST_PASS("remap same VA");

    // read-only mapping: verify flags
    {
        void* phys_page     = pmm_alloc_page();
        unsigned long vaddr = TEST_VA_BASE + 0x8000;
        mmu_map_page(vaddr, V2P(phys_page), MMU_FLAGS_KERNEL_RO);

        unsigned long flags;
        int mapped = mmu_query(vaddr, 0, &flags);
        TEST_ASSERT("RO: mapped", mapped == 1);
        TEST_ASSERT("RO: AP bits set", (flags & MMU_AP_RO) == MMU_AP_RO);
        TEST_ASSERT("RO: PXN set", (flags & MMU_PXN) != 0);

        mmu_unmap_page(vaddr);
        pmm_free_page(phys_page);
    }
    TEST_PASS("read-only flags");

    // fill test: map 16 pages, verify all, unmap all
    {
#define FILL_COUNT 16
        void* pages[FILL_COUNT];
        unsigned long vas[FILL_COUNT];
        for (int i = 0; i < FILL_COUNT; i++)
        {
            pages[i] = pmm_alloc_page();
            vas[i]   = TEST_VA_BASE + 0x10000 + (unsigned long)i * 0x1000;
            mmu_map_page(vas[i], V2P(pages[i]), MMU_FLAGS_KERNEL_RW);
            *(volatile unsigned long*)vas[i] = (unsigned long)(0xF000 + i);
        }

        int ok = 1;
        for (int i = 0; i < FILL_COUNT; i++)
        {
            if (*(volatile unsigned long*)pages[i] != (unsigned long)(0xF000 + i))
                ok = 0;
        }
        TEST_ASSERT("fill: all values correct", ok);

        for (int i = 0; i < FILL_COUNT; i++)
        {
            mmu_unmap_page(vas[i]);
            pmm_free_page(pages[i]);
        }

        // verify all unmapped
        ok = 1;
        for (int i = 0; i < FILL_COUNT; i++)
        {
            if (mmu_query(vas[i], 0, 0))
                ok = 0;
        }
        TEST_ASSERT("fill: all unmapped", ok);
#undef FILL_COUNT
    }
    TEST_PASS("fill 16 pages");

    TEST_SUITE_END("MMU Dynamic Mapping");
}
