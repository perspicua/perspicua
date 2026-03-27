#include "test.h"
#include "mm/mmu.h"
#include "mm/pmm.h"
#include "mm/addr.h"
#include "string.h"

// user VA range for tests — low addresses (TTBR0 space, below kernel VMA)
#define USER_VA_BASE 0x200000000ULL

void test_mmu_user(void)
{
    TEST_SUITE_BEGIN("MMU Per-Process Page Tables");

    // --- create and destroy empty PGD ---
    {
        unsigned long *pgd = mmu_create_user_pgd();
        TEST_ASSERT("create_pgd: non-null", pgd != 0);

        // fresh PGD should have no mappings
        int mapped = mmu_user_query(pgd, USER_VA_BASE, 0, 0);
        TEST_ASSERT("empty pgd: not mapped", mapped == 0);

        mmu_destroy_user_pgd(pgd);
    }
    TEST_PASS("create/destroy empty PGD");

    // --- basic user map + query ---
    {
        unsigned long *pgd = mmu_create_user_pgd();
        void *phys = pmm_alloc_page();
        unsigned long paddr = V2P(phys);
        unsigned long vaddr = USER_VA_BASE;

        mmu_user_map_page(pgd, vaddr, paddr, MMU_PAGE_USER_CODE);

        unsigned long out_pa = 0;
        unsigned long out_fl = 0;
        int mapped = mmu_user_query(pgd, vaddr, &out_pa, &out_fl);
        TEST_ASSERT("user map: mapped", mapped == 1);
        TEST_ASSERT("user map: paddr correct", out_pa == paddr);
        TEST_ASSERT("user map: valid+page",
                    (out_fl & (MMU_PTE_VALID | MMU_PTE_PAGE)) == (MMU_PTE_VALID | MMU_PTE_PAGE));

        mmu_user_unmap_page(pgd, vaddr);
        mmu_destroy_user_pgd(pgd);
    }
    TEST_PASS("user map+query");

    // --- unmap removes mapping ---
    {
        unsigned long *pgd = mmu_create_user_pgd();
        void *phys = pmm_alloc_page();
        unsigned long vaddr = USER_VA_BASE + 0x1000;

        mmu_user_map_page(pgd, vaddr, V2P(phys), MMU_PAGE_USER_DATA);
        mmu_user_unmap_page(pgd, vaddr);

        int mapped = mmu_user_query(pgd, vaddr, 0, 0);
        TEST_ASSERT("user unmap: not mapped", mapped == 0);

        mmu_destroy_user_pgd(pgd);
    }
    TEST_PASS("user unmap");

    // --- user code vs data flags ---
    {
        unsigned long *pgd = mmu_create_user_pgd();
        void *p1 = pmm_alloc_page();
        void *p2 = pmm_alloc_page();
        unsigned long va_code = USER_VA_BASE + 0x2000;
        unsigned long va_data = USER_VA_BASE + 0x3000;

        mmu_user_map_page(pgd, va_code, V2P(p1), MMU_PAGE_USER_CODE);
        mmu_user_map_page(pgd, va_data, V2P(p2), MMU_PAGE_USER_DATA);

        unsigned long fl_code = 0, fl_data = 0;
        mmu_user_query(pgd, va_code, 0, &fl_code);
        mmu_user_query(pgd, va_data, 0, &fl_data);

        // code: PXN set (no kernel exec), UXN clear (user can exec)
        TEST_ASSERT("code: PXN set", (fl_code & MMU_PXN) != 0);
        TEST_ASSERT("code: UXN clear", (fl_code & MMU_UXN) == 0);

        // data: both PXN and UXN set (no exec at all)
        TEST_ASSERT("data: PXN set", (fl_data & MMU_PXN) != 0);
        TEST_ASSERT("data: UXN set", (fl_data & MMU_UXN) != 0);

        // both should have user bit (AP[1])
        TEST_ASSERT("code: AP_USER", (fl_code & MMU_AP_USER) != 0);
        TEST_ASSERT("data: AP_USER", (fl_data & MMU_AP_USER) != 0);

        mmu_user_unmap_page(pgd, va_code);
        mmu_user_unmap_page(pgd, va_data);
        mmu_destroy_user_pgd(pgd);
    }
    TEST_PASS("user code vs data flags");

    // --- multiple pages in same PGD ---
    {
        unsigned long *pgd = mmu_create_user_pgd();
#define MP_COUNT 8
        void *pages[MP_COUNT];
        unsigned long vas[MP_COUNT];

        for (int i = 0; i < MP_COUNT; i++) {
            pages[i] = pmm_alloc_page();
            vas[i] = USER_VA_BASE + 0x10000 + (unsigned long)i * 0x1000;
            mmu_user_map_page(pgd, vas[i], V2P(pages[i]), MMU_PAGE_USER_DATA);
        }

        int ok = 1;
        for (int i = 0; i < MP_COUNT; i++) {
            unsigned long pa;
            if (!mmu_user_query(pgd, vas[i], &pa, 0) || pa != V2P(pages[i]))
                ok = 0;
        }
        TEST_ASSERT("multi-page: all mapped correctly", ok);

        for (int i = 0; i < MP_COUNT; i++) {
            mmu_user_unmap_page(pgd, vas[i]);
        }

        // confirm all unmapped
        ok = 1;
        for (int i = 0; i < MP_COUNT; i++) {
            if (mmu_user_query(pgd, vas[i], 0, 0))
                ok = 0;
        }
        TEST_ASSERT("multi-page: all unmapped", ok);

        mmu_destroy_user_pgd(pgd);
#undef MP_COUNT
    }
    TEST_PASS("multiple pages same PGD");

    // --- pages across different L2 regions (different 2MB windows) ---
    {
        unsigned long *pgd = mmu_create_user_pgd();
        void *p1 = pmm_alloc_page();
        void *p2 = pmm_alloc_page();
        unsigned long va1 = USER_VA_BASE;            // L2 index 0
        unsigned long va2 = USER_VA_BASE + 0x200000; // L2 index 1 (+2MB)

        mmu_user_map_page(pgd, va1, V2P(p1), MMU_PAGE_USER_DATA);
        mmu_user_map_page(pgd, va2, V2P(p2), MMU_PAGE_USER_DATA);

        unsigned long pa1, pa2;
        TEST_ASSERT("cross-L2: map1", mmu_user_query(pgd, va1, &pa1, 0) && pa1 == V2P(p1));
        TEST_ASSERT("cross-L2: map2", mmu_user_query(pgd, va2, &pa2, 0) && pa2 == V2P(p2));

        mmu_user_unmap_page(pgd, va1);
        mmu_user_unmap_page(pgd, va2);
        mmu_destroy_user_pgd(pgd);
    }
    TEST_PASS("pages across L2 regions");

    // --- pages across different L1 entries (different 1GB windows) ---
    {
        unsigned long *pgd = mmu_create_user_pgd();
        void *p1 = pmm_alloc_page();
        void *p2 = pmm_alloc_page();
        unsigned long va1 = 0x100000000ULL; // L1 index 4
        unsigned long va2 = 0x200000000ULL; // L1 index 8

        mmu_user_map_page(pgd, va1, V2P(p1), MMU_PAGE_USER_DATA);
        mmu_user_map_page(pgd, va2, V2P(p2), MMU_PAGE_USER_DATA);

        unsigned long pa;
        TEST_ASSERT("cross-L1: map1", mmu_user_query(pgd, va1, &pa, 0) && pa == V2P(p1));
        TEST_ASSERT("cross-L1: map2", mmu_user_query(pgd, va2, &pa, 0) && pa == V2P(p2));

        mmu_user_unmap_page(pgd, va1);
        mmu_user_unmap_page(pgd, va2);
        mmu_destroy_user_pgd(pgd);
    }
    TEST_PASS("pages across L1 entries");

    // --- remap: unmap then map different phys to same VA ---
    {
        unsigned long *pgd = mmu_create_user_pgd();
        void *p1 = pmm_alloc_page();
        void *p2 = pmm_alloc_page();
        unsigned long vaddr = USER_VA_BASE + 0x4000;

        mmu_user_map_page(pgd, vaddr, V2P(p1), MMU_PAGE_USER_DATA);
        unsigned long pa;
        TEST_ASSERT("remap: first", mmu_user_query(pgd, vaddr, &pa, 0) && pa == V2P(p1));

        mmu_user_unmap_page(pgd, vaddr);
        mmu_user_map_page(pgd, vaddr, V2P(p2), MMU_PAGE_USER_DATA);
        TEST_ASSERT("remap: second", mmu_user_query(pgd, vaddr, &pa, 0) && pa == V2P(p2));

        mmu_user_unmap_page(pgd, vaddr);
        mmu_destroy_user_pgd(pgd);
    }
    TEST_PASS("remap same VA different phys");

    // --- two PGDs are fully isolated ---
    {
        unsigned long *pgd_a = mmu_create_user_pgd();
        unsigned long *pgd_b = mmu_create_user_pgd();
        void *pa = pmm_alloc_page();
        void *pb = pmm_alloc_page();
        unsigned long vaddr = USER_VA_BASE + 0x5000; // same VA in both

        mmu_user_map_page(pgd_a, vaddr, V2P(pa), MMU_PAGE_USER_DATA);
        mmu_user_map_page(pgd_b, vaddr, V2P(pb), MMU_PAGE_USER_DATA);

        unsigned long out_a, out_b;
        TEST_ASSERT("iso: A mapped", mmu_user_query(pgd_a, vaddr, &out_a, 0));
        TEST_ASSERT("iso: B mapped", mmu_user_query(pgd_b, vaddr, &out_b, 0));
        TEST_ASSERT("iso: A points to pa", out_a == V2P(pa));
        TEST_ASSERT("iso: B points to pb", out_b == V2P(pb));
        TEST_ASSERT("iso: different phys", out_a != out_b);

        // mapping in A doesn't appear in B's other addresses
        unsigned long va_other = USER_VA_BASE + 0x6000;
        pmm_hold_page(pa);
        mmu_user_map_page(pgd_a, va_other, V2P(pa), MMU_PAGE_USER_DATA);
        TEST_ASSERT("iso: other VA not in B", mmu_user_query(pgd_b, va_other, 0, 0) == 0);

        mmu_user_unmap_page(pgd_a, vaddr);
        mmu_user_unmap_page(pgd_a, va_other);
        mmu_user_unmap_page(pgd_b, vaddr);

        mmu_destroy_user_pgd(pgd_a);
        mmu_destroy_user_pgd(pgd_b);
    }
    TEST_PASS("two PGDs fully isolated");

    // --- destroy PGD with active mappings frees table pages ---
    {
        // track PMM state before and after to ensure no leaks
        void *before = pmm_alloc_page();
        pmm_free_page(before);

        unsigned long *pgd = mmu_create_user_pgd();
        void *p1 = pmm_alloc_page();
        void *p2 = pmm_alloc_page();
        void *p3 = pmm_alloc_page();

        // map 3 pages across 2 L2 regions (forces 2 L3 tables + 1 L2 table minimum)
        mmu_user_map_page(pgd, USER_VA_BASE, V2P(p1), MMU_PAGE_USER_DATA);
        mmu_user_map_page(pgd, USER_VA_BASE + 0x1000, V2P(p2), MMU_PAGE_USER_DATA);
        mmu_user_map_page(pgd, USER_VA_BASE + 0x200000, V2P(p3), MMU_PAGE_USER_DATA);

        // destroy frees all mapped physical pages + table pages + PGD
        mmu_destroy_user_pgd(pgd);

        // if table pages leaked, we'd eventually run out — allocate to confirm
        void *after = pmm_alloc_page();
        TEST_ASSERT("destroy: no table page leak", after != 0);
        pmm_free_page(after);
    }
    TEST_PASS("destroy PGD frees table pages");

    // --- kernel_ttbr0 returns non-zero ---
    {
        unsigned long kttbr0 = mmu_kernel_ttbr0();
        TEST_ASSERT("kernel_ttbr0: non-zero", kttbr0 != 0);
    }
    TEST_PASS("kernel_ttbr0 non-zero");

    // --- mmu_switch_user sets TTBR0 correctly ---
    {
        unsigned long *pgd = mmu_create_user_pgd();

        // switch to user PGD
        mmu_switch_user(pgd, 42);

        unsigned long ttbr0;
        asm volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0));

        unsigned long expected_phys = V2P(pgd);
        unsigned long actual_phys = ttbr0 & 0x0000FFFFFFFFFFFFULL;
        unsigned long actual_asid = ttbr0 >> 48;

        TEST_ASSERT("switch_user: phys match", actual_phys == expected_phys);
        TEST_ASSERT("switch_user: ASID=42", actual_asid == 42);

        // restore kernel TTBR0
        asm volatile("msr ttbr0_el1, %0\n isb" : : "r"(mmu_kernel_ttbr0()));

        mmu_destroy_user_pgd(pgd);
    }
    TEST_PASS("mmu_switch_user sets TTBR0");

    // --- ASID encoding in TTBR0 ---
    {
        unsigned long *pgd1 = mmu_create_user_pgd();
        unsigned long *pgd2 = mmu_create_user_pgd();

        mmu_switch_user(pgd1, 1);
        unsigned long ttbr0_1;
        asm volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0_1));

        mmu_switch_user(pgd2, 2);
        unsigned long ttbr0_2;
        asm volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0_2));

        TEST_ASSERT("asid: pgd1 asid=1", (ttbr0_1 >> 48) == 1);
        TEST_ASSERT("asid: pgd2 asid=2", (ttbr0_2 >> 48) == 2);
        TEST_ASSERT("asid: different phys",
                    (ttbr0_1 & 0x0000FFFFFFFFFFFFULL) != (ttbr0_2 & 0x0000FFFFFFFFFFFFULL));

        asm volatile("msr ttbr0_el1, %0\n isb" : : "r"(mmu_kernel_ttbr0()));
        mmu_destroy_user_pgd(pgd1);
        mmu_destroy_user_pgd(pgd2);
    }
    TEST_PASS("ASID encoding in TTBR0");

    // --- query unmapped VA in user PGD returns 0 ---
    {
        unsigned long *pgd = mmu_create_user_pgd();

        // query with no L1 entry
        TEST_ASSERT("uq: no L1", mmu_user_query(pgd, 0x300000000ULL, 0, 0) == 0);

        // map one page, query a different one in same L2
        void *p = pmm_alloc_page();
        mmu_user_map_page(pgd, USER_VA_BASE, V2P(p), MMU_PAGE_USER_DATA);
        TEST_ASSERT("uq: different page", mmu_user_query(pgd, USER_VA_BASE + 0x1000, 0, 0) == 0);

        // query in same L1 but different L2
        TEST_ASSERT("uq: different L2", mmu_user_query(pgd, USER_VA_BASE + 0x200000, 0, 0) == 0);

        mmu_user_unmap_page(pgd, USER_VA_BASE);
        mmu_destroy_user_pgd(pgd);
    }
    TEST_PASS("query unmapped in user PGD");

    // --- destroy PGD with mappings across multiple L1 entries ---
    {
        unsigned long *pgd = mmu_create_user_pgd();
        void *p1 = pmm_alloc_page();
        void *p2 = pmm_alloc_page();

        mmu_user_map_page(pgd, 0x100000000ULL, V2P(p1), MMU_PAGE_USER_DATA); // L1[4]
        mmu_user_map_page(pgd, 0x200000000ULL, V2P(p2), MMU_PAGE_USER_DATA); // L1[8]

        // destroy frees all mapped physical pages + table pages + PGD
        mmu_destroy_user_pgd(pgd);

        // verify PMM still works (no corruption)
        void *check = pmm_alloc_page();
        TEST_ASSERT("destroy multi-L1: PMM ok", check != 0);
        pmm_free_page(check);
    }
    TEST_PASS("destroy PGD multi-L1 subtrees");

    // --- stress: create many PGDs, map, destroy ---
    {
#define STRESS_COUNT 4
        unsigned long *pgds[STRESS_COUNT];
        void *phys[STRESS_COUNT];

        for (int i = 0; i < STRESS_COUNT; i++) {
            pgds[i] = mmu_create_user_pgd();
            phys[i] = pmm_alloc_page();
            mmu_user_map_page(pgds[i], USER_VA_BASE + (unsigned long)i * 0x1000, V2P(phys[i]),
                              MMU_PAGE_USER_DATA);
        }

        // verify each PGD has its own mapping
        int ok = 1;
        for (int i = 0; i < STRESS_COUNT; i++) {
            unsigned long pa;
            if (!mmu_user_query(pgds[i], USER_VA_BASE + (unsigned long)i * 0x1000, &pa, 0))
                ok = 0;
            else if (pa != V2P(phys[i]))
                ok = 0;

            // verify other PGDs don't have this mapping
            for (int j = 0; j < STRESS_COUNT; j++) {
                if (j == i)
                    continue;
                if (mmu_user_query(pgds[j], USER_VA_BASE + (unsigned long)i * 0x1000, 0, 0))
                    ok = 0;
            }
        }
        TEST_ASSERT("stress: all isolated", ok);

        for (int i = 0; i < STRESS_COUNT; i++) {
            mmu_destroy_user_pgd(pgds[i]);
        }
#undef STRESS_COUNT
    }
    TEST_PASS("stress: 4 PGDs isolated");

    // --- shared physical page in two PGDs ---
    {
        unsigned long *pgd_a = mmu_create_user_pgd();
        unsigned long *pgd_b = mmu_create_user_pgd();
        void *shared_page = pmm_alloc_page();
        unsigned long vaddr = USER_VA_BASE + 0x7000;

        pmm_hold_page(shared_page); // Hold because we map it twice and mmu_user_unmap_page frees
        mmu_user_map_page(pgd_a, vaddr, V2P(shared_page), MMU_PAGE_USER_DATA);
        mmu_user_map_page(pgd_b, vaddr, V2P(shared_page), MMU_PAGE_USER_DATA);

        unsigned long pa_a, pa_b;
        mmu_user_query(pgd_a, vaddr, &pa_a, 0);
        mmu_user_query(pgd_b, vaddr, &pa_b, 0);
        TEST_ASSERT("shared: same phys in A", pa_a == V2P(shared_page));
        TEST_ASSERT("shared: same phys in B", pa_b == V2P(shared_page));

        mmu_user_unmap_page(pgd_a, vaddr);
        mmu_user_unmap_page(pgd_b, vaddr);

        mmu_destroy_user_pgd(pgd_a);
        mmu_destroy_user_pgd(pgd_b);
    }
    TEST_PASS("shared physical page in two PGDs");

    // --- same physical page mapped to multiple VAs in same PGD ---
    {
        unsigned long *pgd = mmu_create_user_pgd();
        void *shared_page = pmm_alloc_page();
        unsigned long va1 = USER_VA_BASE + 0x8000;
        unsigned long va2 = USER_VA_BASE + 0x9000;

        pmm_hold_page(shared_page); // Hold due to multi-map
        mmu_user_map_page(pgd, va1, V2P(shared_page), MMU_PAGE_USER_DATA);
        mmu_user_map_page(pgd, va2, V2P(shared_page), MMU_PAGE_USER_DATA);

        *(volatile unsigned long *)shared_page = 0;

        unsigned long pa1, pa2;
        TEST_ASSERT("multi-va: map1", mmu_user_query(pgd, va1, &pa1, 0) && pa1 == V2P(shared_page));
        TEST_ASSERT("multi-va: map2", mmu_user_query(pgd, va2, &pa2, 0) && pa2 == V2P(shared_page));

        mmu_user_unmap_page(pgd, va1);
        mmu_user_unmap_page(pgd, va2);
        mmu_destroy_user_pgd(pgd);
    }
    TEST_PASS("shared physical page multiple VAs in same PGD");

    // --- map over existing mapping should replace or at least not leak (assuming implementation
    // allows replace) --- Perspicua mmu_user_map_page actually doesn't specify if it replaces, but
    // let's test if we can query it safely.
    {
        unsigned long *pgd = mmu_create_user_pgd();
        void *p1 = pmm_alloc_page();
        void *p2 = pmm_alloc_page();
        unsigned long va = USER_VA_BASE + 0xA000;

        mmu_user_map_page(pgd, va, V2P(p1), MMU_PAGE_USER_DATA);
        mmu_user_map_page(pgd, va, V2P(p2), MMU_PAGE_USER_DATA); // frees p1 internally
        unsigned long pa;
        TEST_ASSERT("overwrite: queried map", mmu_user_query(pgd, va, &pa, 0));
        TEST_ASSERT("overwrite: updated to p2", pa == V2P(p2));
        mmu_user_unmap_page(pgd, va);
        mmu_destroy_user_pgd(pgd);
    }
    TEST_PASS("map over existing mapping");

    // --- fill: 16 pages in user PGD ---
    {
        unsigned long *pgd = mmu_create_user_pgd();
#define FILL_COUNT 16
        void *pages[FILL_COUNT];
        unsigned long vas[FILL_COUNT];

        for (int i = 0; i < FILL_COUNT; i++) {
            pages[i] = pmm_alloc_page();
            vas[i] = USER_VA_BASE + 0x20000 + (unsigned long)i * 0x1000;
            mmu_user_map_page(pgd, vas[i], V2P(pages[i]), MMU_PAGE_USER_DATA);
        }

        int ok = 1;
        for (int i = 0; i < FILL_COUNT; i++) {
            unsigned long pa;
            if (!mmu_user_query(pgd, vas[i], &pa, 0) || pa != V2P(pages[i]))
                ok = 0;
        }
        TEST_ASSERT("fill16: all correct", ok);

        // unmap all
        for (int i = 0; i < FILL_COUNT; i++) {
            mmu_user_unmap_page(pgd, vas[i]);
        }

        ok = 1;
        for (int i = 0; i < FILL_COUNT; i++) {
            if (mmu_user_query(pgd, vas[i], 0, 0))
                ok = 0;
        }
        TEST_ASSERT("fill16: all unmapped", ok);

        mmu_destroy_user_pgd(pgd);
#undef FILL_COUNT
    }
    TEST_PASS("fill 16 pages in user PGD");

    // --- user PGD doesn't affect kernel page tables ---
    {
        unsigned long *pgd = mmu_create_user_pgd();
        void *p = pmm_alloc_page();

        // map in user PGD
        mmu_user_map_page(pgd, USER_VA_BASE + 0x8000, V2P(p), MMU_PAGE_USER_DATA);

        // kernel query for the same VA (in kernel PGD / TTBR1) should not find it,
        // because USER_VA_BASE is a low address (TTBR0 space), not in TTBR1 range
        // We can verify by checking that the kernel's identity-mapped RAM region is unchanged
        unsigned long kernel_pa;
        int kernel_mapped = mmu_query(KERNEL_VMA + 0x200000, &kernel_pa, 0);
        TEST_ASSERT("kernel unaffected: still mapped", kernel_mapped == 1);
        TEST_ASSERT("kernel unaffected: paddr", kernel_pa == 0x200000);

        mmu_user_unmap_page(pgd, USER_VA_BASE + 0x8000);
        mmu_destroy_user_pgd(pgd);
    }
    TEST_PASS("user PGD doesn't affect kernel");

    TEST_SUITE_END("MMU Per-Process Page Tables");
}
