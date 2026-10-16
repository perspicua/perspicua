#include "test.h"
#include "mm/mmu.h"
#include "mm/pmm.h"
#include "mm/addr.h"
#include "sched/sched.h"
#include "string.h"

#define TEST_VA_BASE (KERNEL_VMA + 0x100000000ULL) // PGD index 4

// Returns 0 when a byte at addr can be read and -1 when the read faults; the fixup table resumes it.
int test_probe_read(const void *addr);
asm(".pushsection .text\n"
    ".balign 4\n"
    ".global test_probe_read\n"
    ".type test_probe_read, %function\n"
    "test_probe_read:\n"
    "1:  ldrb w1, [x0]\n"
    "    mov w0, #0\n"
    "    ret\n"
    "2:  mov w0, #-1\n"
    "    ret\n"
    ".size test_probe_read, . - test_probe_read\n"
    ".pushsection __ex_table, \"a\"\n"
    ".align 3\n"
    ".quad 1b, 2b\n"
    ".popsection\n"
    ".popsection\n");

void test_mmu(void)
{
    TEST_SUITE_BEGIN("MMU Dynamic Mapping");

    // basic map and query
    {
        void *phys_page = pmm_alloc_page();
        unsigned long paddr = V2P(phys_page);
        unsigned long vaddr = TEST_VA_BASE;

        mmu_map_page(vaddr, paddr, MMU_FLAGS_KERNEL_RW);

        unsigned long out_paddr = 0;
        unsigned long out_flags = 0;
        int mapped = mmu_query(vaddr, &out_paddr, &out_flags);
        TEST_ASSERT("map: query returns mapped", mapped == 1);
        TEST_ASSERT("map: paddr correct", out_paddr == paddr);
        TEST_ASSERT("map: valid+page bits set",
                    (out_flags & (MMU_PTE_VALID | MMU_PTE_PAGE)) == (MMU_PTE_VALID | MMU_PTE_PAGE));

        mmu_unmap_page(vaddr);
        pmm_free_page(phys_page);
    }

    // unmap makes page not-mapped
    {
        void *phys_page = pmm_alloc_page();
        unsigned long paddr = V2P(phys_page);
        unsigned long vaddr = TEST_VA_BASE + 0x1000; // next page

        mmu_map_page(vaddr, paddr, MMU_FLAGS_KERNEL_RW);
        mmu_unmap_page(vaddr);

        int mapped = mmu_query(vaddr, 0, 0);
        TEST_ASSERT("unmap: query returns unmapped", mapped == 0);

        pmm_free_page(phys_page);
    }

    // query unmapped address returns 0
    {
        // PGD index 5, never mapped
        unsigned long vaddr = KERNEL_VMA + 0x140000000ULL;
        int mapped = mmu_query(vaddr, 0, 0);
        TEST_ASSERT("query unmapped returns 0", mapped == 0);
    }

    // map, write, read through new mapping
    {
        void *phys_page = pmm_alloc_page();
        unsigned long paddr = V2P(phys_page);
        unsigned long vaddr = TEST_VA_BASE + 0x2000;

        mmu_map_page(vaddr, paddr, MMU_FLAGS_KERNEL_RW);

        // write through virtual address
        volatile unsigned long *ptr = (volatile unsigned long *)vaddr;
        *ptr = 0xCAFEBABEDEADBEEFULL;

        // read through the original kernel-mapped VA (P2V of the same phys page)
        volatile unsigned long *direct = (volatile unsigned long *)phys_page;
        TEST_ASSERT("write through mapping", *direct == 0xCAFEBABEDEADBEEFULL);

        mmu_unmap_page(vaddr);
        pmm_free_page(phys_page);
    }

    // map multiple pages in same L2 region
    {
        void *p1 = pmm_alloc_page();
        void *p2 = pmm_alloc_page();
        void *p3 = pmm_alloc_page();
        unsigned long v1 = TEST_VA_BASE + 0x3000;
        unsigned long v2 = TEST_VA_BASE + 0x4000;
        unsigned long v3 = TEST_VA_BASE + 0x5000;

        mmu_map_page(v1, V2P(p1), MMU_FLAGS_KERNEL_RW);
        mmu_map_page(v2, V2P(p2), MMU_FLAGS_KERNEL_RW);
        mmu_map_page(v3, V2P(p3), MMU_FLAGS_KERNEL_RW);

        // write distinct values
        *(volatile unsigned long *)v1 = 0x1111;
        *(volatile unsigned long *)v2 = 0x2222;
        *(volatile unsigned long *)v3 = 0x3333;

        TEST_ASSERT("multi-page: p1 value", *(volatile unsigned long *)p1 == 0x1111);
        TEST_ASSERT("multi-page: p2 value", *(volatile unsigned long *)p2 == 0x2222);
        TEST_ASSERT("multi-page: p3 value", *(volatile unsigned long *)p3 == 0x3333);

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

    // map pages across different L2 entries (different 2MB regions)
    {
        void *p1 = pmm_alloc_page();
        void *p2 = pmm_alloc_page();
        // v1 in first 2MB of PGD[4], v2 in second 2MB
        unsigned long v1 = TEST_VA_BASE + 0x6000;
        unsigned long v2 = TEST_VA_BASE + 0x200000 + 0x6000; // +2MB

        mmu_map_page(v1, V2P(p1), MMU_FLAGS_KERNEL_RW);
        mmu_map_page(v2, V2P(p2), MMU_FLAGS_KERNEL_RW);

        *(volatile unsigned long *)v1 = 0xAAAA;
        *(volatile unsigned long *)v2 = 0xBBBB;

        TEST_ASSERT("cross-L2: p1", *(volatile unsigned long *)p1 == 0xAAAA);
        TEST_ASSERT("cross-L2: p2", *(volatile unsigned long *)p2 == 0xBBBB);

        mmu_unmap_page(v1);
        mmu_unmap_page(v2);
        pmm_free_page(p1);
        pmm_free_page(p2);
    }

    // the linear map translates a kernel VA in the first GB
    {
        unsigned long vaddr = KERNEL_VMA + 0x200000;
        unsigned long pa;
        int mapped = mmu_query(vaddr, &pa, 0);
        TEST_ASSERT("linear map: mapped", mapped == 1);
        TEST_ASSERT("linear map: paddr", pa == 0x200000);
    }

    // RAM above the first GB gets 4 KB pages; only regions with no RAM stay 2 MB blocks
    {
        static const struct pmm_range fake_ram[] = {
            {0x40000000UL, 0x40400000UL},
            {0x40500000UL, 0x40501000UL},
            {0x7FF00000UL, 0x80000000UL},
        };
        unsigned long free_before = pmm_get_free_pages();
        unsigned long *l2t = pmm_alloc_page();
        mmu_test_map_linear_gb(l2t, 0x40000000UL, fake_ram, 3);

        TEST_ASSERT_EQ("linear map: RAM region is a table", l2t[0] & 3, 3);
        TEST_ASSERT_EQ("linear map: second RAM region is a table", l2t[1] & 3, 3);
        TEST_ASSERT_EQ("linear map: partly-RAM region is a table", l2t[2] & 3, 3);
        TEST_ASSERT_EQ("linear map: RAM at the top of the GB is a table", l2t[511] & 3, 3);
        TEST_ASSERT_EQ("linear map: region without RAM is a block", l2t[3] & 3, 1);
        TEST_ASSERT_EQ("linear map: block output address", l2t[3] & MMU_PTE_ADDR_MASK,
                       0x40600000UL);

        unsigned long *l3 = (unsigned long *)P2V(l2t[0] & MMU_PTE_ADDR_MASK);
        TEST_ASSERT_EQ("linear map: page output address", l3[5] & MMU_PTE_ADDR_MASK, 0x40005000UL);
        TEST_ASSERT_EQ("linear map: page descriptor", l3[5] & 3, 3);
        TEST_ASSERT("linear map: page is writable and never executable",
                    !(l3[5] & MMU_AP_RO) && (l3[5] & MMU_XN) == MMU_XN);

        for (int i = 0; i < 512; i++) {
            if ((l2t[i] & 3) == 3) {
                pmm_free_page((void *)P2V(l2t[i] & MMU_PTE_ADDR_MASK));
            }
        }
        pmm_free_page(l2t);
        TEST_ASSERT_EQ("linear map: tables freed", pmm_get_free_pages(), free_before);
    }

    // no RAM range the pmm found sits behind a 2 MB block, so no guard-page unmap splits one
    {
        const struct pmm_range *ram;
        int ram_count = pmm_get_ram_ranges(&ram);
        int blocks = 0;
        for (int i = 0; i < ram_count; i++) {
            unsigned long pa = ram[i].start & ~(0x200000UL - 1);
            for (; pa < ram[i].end; pa += 0x200000UL) {
                blocks += mmu_test_is_block_entry(P2V(pa));
            }
        }
        TEST_ASSERT_EQ("linear map: no RAM behind a block", blocks, 0);
    }

    // map and unmap cycle — re-map same VA to different phys
    {
        void *p1 = pmm_alloc_page();
        void *p2 = pmm_alloc_page();
        unsigned long vaddr = TEST_VA_BASE + 0x7000;

        mmu_map_page(vaddr, V2P(p1), MMU_FLAGS_KERNEL_RW);
        *(volatile unsigned long *)vaddr = 0x1234;
        TEST_ASSERT("remap: first mapping", *(volatile unsigned long *)p1 == 0x1234);
        mmu_unmap_page(vaddr);

        mmu_map_page(vaddr, V2P(p2), MMU_FLAGS_KERNEL_RW);
        *(volatile unsigned long *)vaddr = 0x5678;
        TEST_ASSERT("remap: second mapping", *(volatile unsigned long *)p2 == 0x5678);
        // p1 should still have old value
        TEST_ASSERT("remap: p1 unchanged", *(volatile unsigned long *)p1 == 0x1234);
        mmu_unmap_page(vaddr);

        pmm_free_page(p1);
        pmm_free_page(p2);
    }

    // read-only mapping: verify flags
    {
        void *phys_page = pmm_alloc_page();
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

    // fill test: map 16 pages, verify all, unmap all
    {
#define FILL_COUNT 16
        void *pages[FILL_COUNT];
        unsigned long vas[FILL_COUNT];
        for (int i = 0; i < FILL_COUNT; i++) {
            pages[i] = pmm_alloc_page();
            vas[i] = TEST_VA_BASE + 0x10000 + (unsigned long)i * 0x1000;
            mmu_map_page(vas[i], V2P(pages[i]), MMU_FLAGS_KERNEL_RW);
            *(volatile unsigned long *)vas[i] = (unsigned long)(0xF000 + i);
        }

        int ok = 1;
        for (int i = 0; i < FILL_COUNT; i++) {
            if (*(volatile unsigned long *)pages[i] != (unsigned long)(0xF000 + i)) {
                ok = 0;
            }
        }
        TEST_ASSERT("fill: all values correct", ok);

        for (int i = 0; i < FILL_COUNT; i++) {
            mmu_unmap_page(vas[i]);
            pmm_free_page(pages[i]);
        }

        // verify all unmapped
        ok = 1;
        for (int i = 0; i < FILL_COUNT; i++) {
            if (mmu_query(vas[i], 0, 0)) {
                ok = 0;
            }
        }
        TEST_ASSERT("fill: all unmapped", ok);
#undef FILL_COUNT
    }

    // unmapping flushes the TLB: a page read just before it is unmapped faults just after
    {
        void *page = pmm_alloc_page();
        unsigned long vaddr = TEST_VA_BASE + 0x40000;
        mmu_map_page(vaddr, V2P(page), MMU_FLAGS_KERNEL_RW);

        int before = test_probe_read((const void *)vaddr);
        mmu_unmap_page(vaddr);
        int after = test_probe_read((const void *)vaddr);
        pmm_free_page(page);

        TEST_ASSERT_EQ("probe: a mapped page reads", before, 0);
        TEST_ASSERT_EQ("unmap: a page read before the unmap faults after it", after, -1);
    }

    // a kernel stack's guard page faults although allocating the stack has just written to it
    {
        unsigned char *stack = kstack_alloc();
        int guard = stack ? test_probe_read(stack) : 0;
        int body = stack ? test_probe_read(stack + PAGE_SIZE) : -1;
        kstack_free(stack);

        TEST_ASSERT("kstack: allocated", stack != NULL);
        TEST_ASSERT_EQ("kstack: the guard page faults", guard, -1);
        TEST_ASSERT_EQ("kstack: the page above it reads", body, 0);
    }

    TEST_SUITE_END("MMU Dynamic Mapping");
}
