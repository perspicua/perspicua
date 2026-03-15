#include "test.h"
#include "pmm.h"
#include "string.h"

void test_pmm(void)
{
    TEST_SUITE_BEGIN("Physical Memory Manager");

    // basic functionality

    // single page alloc
    {
        void* p = pmm_alloc_page();
        TEST_ASSERT("alloc non-null", p != NULL);
        TEST_ASSERT("page aligned", ((unsigned long)p & (PAGE_SIZE - 1)) == 0);
        pmm_free_page(p);
    }
    TEST_PASS("single page alloc");

    // pmm_alloc_pages(1) equivalent
    {
        void* p = pmm_alloc_pages(1);
        TEST_ASSERT("alloc_pages(1) non-null", p != NULL);
        TEST_ASSERT("alloc_pages(1) aligned", ((unsigned long)p & (PAGE_SIZE - 1)) == 0);
        pmm_free_pages(p, 1);
    }
    TEST_PASS("alloc_pages(1)");

    // zero-count returns null
    TEST_ASSERT("alloc_pages(0) is null", pmm_alloc_pages(0) == NULL);
    TEST_PASS("zero-count returns NULL");

    // free null is safe
    pmm_free_page(NULL);
    pmm_free_pages(NULL, 4);
    TEST_PASS("free(NULL) safe");

    // writable page
    {
        volatile unsigned long* p = (volatile unsigned long*)pmm_alloc_page();
        TEST_ASSERT("write alloc", p != NULL);
        *p = 0xDEADBEEFCAFEBABEUL;
        TEST_ASSERT("write verify", *p == 0xDEADBEEFCAFEBABEUL);
        pmm_free_page((void*)p);
    }
    TEST_PASS("page writable");

    // distinct allocations

    // two single pages are distinct
    {
        void* a = pmm_alloc_page();
        void* b = pmm_alloc_page();
        TEST_ASSERT("a non-null", a != NULL);
        TEST_ASSERT("b non-null", b != NULL);
        TEST_ASSERT("a != b", a != b);
        TEST_ASSERT("a aligned", ((unsigned long)a & (PAGE_SIZE - 1)) == 0);
        TEST_ASSERT("b aligned", ((unsigned long)b & (PAGE_SIZE - 1)) == 0);
        pmm_free_page(a);
        pmm_free_page(b);
    }
    TEST_PASS("two pages distinct");

    // many single pages all distinct
    {
        void* pages[64];
        for (int i = 0; i < 64; i++)
        {
            pages[i] = pmm_alloc_page();
            TEST_ASSERT("64x alloc", pages[i] != NULL);
            TEST_ASSERT("64x aligned", ((unsigned long)pages[i] & (PAGE_SIZE - 1)) == 0);
        }
        int distinct = 1;
        for (int i = 0; i < 64 && distinct; i++)
            for (int j = i + 1; j < 64 && distinct; j++)
                if (pages[i] == pages[j])
                    distinct = 0;
        TEST_ASSERT("all 64 distinct", distinct);
        for (int i = 0; i < 64; i++)
            pmm_free_page(pages[i]);
    }
    TEST_PASS("64 pages distinct");

    // multi-page blocks don't overlap
    {
        void* a = pmm_alloc_pages(4);
        void* b = pmm_alloc_pages(4);
        TEST_ASSERT("4p a ok", a != NULL);
        TEST_ASSERT("4p b ok", b != NULL);
        unsigned long a_start = (unsigned long)a;
        unsigned long a_end = a_start + 4 * PAGE_SIZE;
        unsigned long b_start = (unsigned long)b;
        unsigned long b_end = b_start + 4 * PAGE_SIZE;
        TEST_ASSERT("4p no overlap", a_end <= b_start || b_end <= a_start);
        pmm_free_pages(a, 4);
        pmm_free_pages(b, 4);
    }
    TEST_PASS("multi-page no overlap");

    // free and reuse

    // freed page can be re-allocated
    {
        void* p = pmm_alloc_page();
        pmm_free_page(p);
        void* q = pmm_alloc_page();
        TEST_ASSERT("reuse after free", q != NULL);
        pmm_free_page(q);
    }
    TEST_PASS("reuse after free");

    // rapid alloc/free cycle ×50
    {
        for (int i = 0; i < 50; i++)
        {
            void* pg = pmm_alloc_page();
            TEST_ASSERT("rapid alloc", pg != NULL);
            *(volatile unsigned long*)pg = 0xCAFEBABE00000000UL | (unsigned long)i;
            TEST_ASSERT("rapid canary", *(volatile unsigned long*)pg == (0xCAFEBABE00000000UL | (unsigned long)i));
            pmm_free_page(pg);
        }
    }
    TEST_PASS("rapid cycle x50");

    // free in reverse order
    {
        void* pages[8];
        for (int i = 0; i < 8; i++)
            pages[i] = pmm_alloc_page();
        for (int i = 7; i >= 0; i--)
            pmm_free_page(pages[i]);
        // Should still work fine after reverse-order free
        void* p = pmm_alloc_page();
        TEST_ASSERT("post-reverse alloc ok", p != NULL);
        pmm_free_page(p);
    }
    TEST_PASS("reverse-order free");

    // contiguous multi-page (power-of-2 orders)

    // order 0 through order 5 (1, 2, 4, 8, 16, 32 pages)
    {
        for (int order = 0; order <= 5; order++)
        {
            unsigned long count = 1UL << order;
            void* blk = pmm_alloc_pages(count);
            TEST_ASSERT("order alloc", blk != NULL);
            TEST_ASSERT("order aligned", ((unsigned long)blk & (PAGE_SIZE - 1)) == 0);
            // Write first and last byte of each page
            for (unsigned long i = 0; i < count; i++)
            {
                volatile unsigned char* pg = (unsigned char*)blk + i * PAGE_SIZE;
                *pg = (unsigned char)(i & 0xFF);
                TEST_ASSERT("order writable", *pg == (unsigned char)(i & 0xFF));
            }
            pmm_free_pages(blk, count);
        }
    }
    TEST_PASS("orders 0-5 (1..32 pages)");

    // order 6 = 64 pages (256 kb)
    {
        void* blk = pmm_alloc_pages(64);
        TEST_ASSERT("64-page alloc", blk != NULL);
        TEST_ASSERT("64-page aligned", ((unsigned long)blk & (PAGE_SIZE - 1)) == 0);
        // Touch first, middle, last pages
        *(volatile unsigned char*)blk = 0xAA;
        *((volatile unsigned char*)blk + 32 * PAGE_SIZE) = 0xBB;
        *((volatile unsigned char*)blk + 63 * PAGE_SIZE) = 0xCC;
        TEST_ASSERT("64p first", *(volatile unsigned char*)blk == 0xAA);
        TEST_ASSERT("64p mid", *((volatile unsigned char*)blk + 32 * PAGE_SIZE) == 0xBB);
        TEST_ASSERT("64p last", *((volatile unsigned char*)blk + 63 * PAGE_SIZE) == 0xCC);
        pmm_free_pages(blk, 64);
    }
    TEST_PASS("64-page alloc (256KB)");

    // order 8 = 256 pages (1 mb)
    {
        void* blk = pmm_alloc_pages(256);
        TEST_ASSERT("256-page alloc", blk != NULL);
        *(volatile unsigned long*)blk = 0x1234567890ABCDEFULL;
        *((volatile unsigned long*)blk + 255 * (PAGE_SIZE / sizeof(unsigned long))) = 0xFEDCBA0987654321ULL;
        TEST_ASSERT("1MB first", *(volatile unsigned long*)blk == 0x1234567890ABCDEFULL);
        pmm_free_pages(blk, 256);
    }
    TEST_PASS("256-page alloc (1MB)");

    // order 10 = 1024 pages (4 mb) — max order
    {
        void* blk = pmm_alloc_pages(1024);
        TEST_ASSERT("max-order alloc", blk != NULL);
        TEST_ASSERT("max-order aligned", ((unsigned long)blk & (PAGE_SIZE - 1)) == 0);
        pmm_free_pages(blk, 1024);
    }
    TEST_PASS("max-order 1024 pages (4MB)");

    // non-power-of-2 counts (rounded up internally by get_order)

    // 3 pages → rounds up to order 2 = 4 pages
    {
        void* blk = pmm_alloc_pages(3);
        TEST_ASSERT("3-page alloc", blk != NULL);
        TEST_ASSERT("3-page aligned", ((unsigned long)blk & (PAGE_SIZE - 1)) == 0);
        // Should be safe to write all 3 requested pages
        for (int i = 0; i < 3; i++)
        {
            volatile unsigned char* pg = (unsigned char*)blk + i * PAGE_SIZE;
            *pg = (unsigned char)i;
        }
        pmm_free_pages(blk, 3);
    }
    TEST_PASS("3-page alloc (order 2)");

    // 5 pages → order 3 = 8 pages
    {
        void* blk = pmm_alloc_pages(5);
        TEST_ASSERT("5-page alloc", blk != NULL);
        for (int i = 0; i < 5; i++)
        {
            volatile unsigned char* pg = (unsigned char*)blk + i * PAGE_SIZE;
            *pg = (unsigned char)(0x50 + i);
            TEST_ASSERT("5p write", *pg == (unsigned char)(0x50 + i));
        }
        pmm_free_pages(blk, 5);
    }
    TEST_PASS("5-page alloc (order 3)");

    // 7, 9, 15, 17 pages
    {
        unsigned long counts[] = {7, 9, 15, 17};
        for (int c = 0; c < 4; c++)
        {
            void* blk = pmm_alloc_pages(counts[c]);
            TEST_ASSERT("odd-count alloc", blk != NULL);
            TEST_ASSERT("odd-count aligned", ((unsigned long)blk & (PAGE_SIZE - 1)) == 0);
            pmm_free_pages(blk, counts[c]);
        }
    }
    TEST_PASS("non-power-of-2 counts");

    // buddy merging

    // two single pages free → can alloc 2-page block
    {
        void* a = pmm_alloc_page();
        void* b = pmm_alloc_page();
        pmm_free_page(a);
        pmm_free_page(b);
        void* pair = pmm_alloc_pages(2);
        TEST_ASSERT("buddy merge 2p", pair != NULL);
        TEST_ASSERT("buddy merge aligned", ((unsigned long)pair & (PAGE_SIZE - 1)) == 0);
        pmm_free_pages(pair, 2);
    }
    TEST_PASS("buddy merge 2 pages");

    // four pages free → should merge up to order 2
    {
        void* pages[4];
        for (int i = 0; i < 4; i++)
            pages[i] = pmm_alloc_page();
        for (int i = 0; i < 4; i++)
            pmm_free_page(pages[i]);
        void* quad = pmm_alloc_pages(4);
        TEST_ASSERT("buddy merge 4p", quad != NULL);
        pmm_free_pages(quad, 4);
    }
    TEST_PASS("buddy merge 4 pages");

    // eight pages free → merge to order 3
    {
        void* pages[8];
        for (int i = 0; i < 8; i++)
            pages[i] = pmm_alloc_page();
        for (int i = 0; i < 8; i++)
            pmm_free_page(pages[i]);
        void* octet = pmm_alloc_pages(8);
        TEST_ASSERT("buddy merge 8p", octet != NULL);
        pmm_free_pages(octet, 8);
    }
    TEST_PASS("buddy merge 8 pages");

    // ── 6d. partial merge: free 2, keep 1 between → no merge past held page
    {
        void* a = pmm_alloc_page();
        void* b = pmm_alloc_page();
        void* c = pmm_alloc_page();
        // Free a and c but keep b → buddies of a and c may merge with
        // other blocks but a and c cannot merge with each other (b blocks)
        pmm_free_page(a);
        pmm_free_page(c);
        // Allocate 2 pages — should still succeed from elsewhere
        void* pair = pmm_alloc_pages(2);
        TEST_ASSERT("partial merge alloc 2p", pair != NULL);
        pmm_free_pages(pair, 2);
        pmm_free_page(b);
    }
    TEST_PASS("partial buddy merge");

    // free a 2-page block, then re-alloc as 2 singles
    {
        void* blk = pmm_alloc_pages(2);
        pmm_free_pages(blk, 2);
        // The freed order-1 block should be splittable into two order-0's
        void* a = pmm_alloc_page();
        void* b = pmm_alloc_page();
        TEST_ASSERT("split after free a", a != NULL);
        TEST_ASSERT("split after free b", b != NULL);
        TEST_ASSERT("split distinct", a != b);
        pmm_free_page(a);
        pmm_free_page(b);
    }
    TEST_PASS("free 2p then alloc 2x1p");

    // buddy splitting

    // alloc large, free, then alloc small from split
    {
        void* big = pmm_alloc_pages(8);
        pmm_free_pages(big, 8);
        // Allocating 1 page should split the order-3 block down
        void* small = pmm_alloc_page();
        TEST_ASSERT("split-down alloc", small != NULL);
        // Can still alloc more pages from the remainders
        void* small2 = pmm_alloc_page();
        TEST_ASSERT("split-down second", small2 != NULL);
        TEST_ASSERT("split down distinct", small != small2);
        pmm_free_page(small);
        pmm_free_page(small2);
    }
    TEST_PASS("split high-order to low");

    // progressive splitting: alloc 16p, free, alloc 1p
    {
        void* big = pmm_alloc_pages(16);
        pmm_free_pages(big, 16);
        // order-4 block should split 4 times: 16→8→4→2→1
        void* tiny = pmm_alloc_page();
        TEST_ASSERT("deep split", tiny != NULL);
        pmm_free_page(tiny);
    }
    TEST_PASS("deep order-4 split");

    // contiguity verification

    // multi-page block is truly contiguous
    {
        unsigned long count = 8;
        unsigned char* blk = (unsigned char*)pmm_alloc_pages(count);
        TEST_ASSERT("contig alloc", blk != NULL);
        // Write a different byte at the start of each page
        for (unsigned long i = 0; i < count; i++)
            blk[i * PAGE_SIZE] = (unsigned char)(i + 1);
        // Verify all pages
        int ok = 1;
        for (unsigned long i = 0; i < count; i++)
            if (blk[i * PAGE_SIZE] != (unsigned char)(i + 1))
                ok = 0;
        TEST_ASSERT("contig pages intact", ok);
        pmm_free_pages(blk, count);
    }
    TEST_PASS("contiguity 8 pages");

    // fill entire multi-page block
    {
        unsigned long count = 4;
        unsigned char* blk = (unsigned char*)pmm_alloc_pages(count);
        TEST_ASSERT("fill4p alloc", blk != NULL);
        // Fill entire 16KB with pattern
        unsigned long total = count * PAGE_SIZE;
        for (unsigned long i = 0; i < total; i += PAGE_SIZE)
            blk[i] = (unsigned char)((i / PAGE_SIZE) ^ 0xAA);
        int ok = 1;
        for (unsigned long i = 0; i < total; i += PAGE_SIZE)
            if (blk[i] != (unsigned char)((i / PAGE_SIZE) ^ 0xAA))
                ok = 0;
        TEST_ASSERT("fill4p verify", ok);
        pmm_free_pages(blk, count);
    }
    TEST_PASS("fill 4-page block");

    // data isolation

    // writes to one page don't affect another
    {
        unsigned long* a = (unsigned long*)pmm_alloc_page();
        unsigned long* b = (unsigned long*)pmm_alloc_page();
        // Fill a with 0x1111... and b with 0x2222...
        for (int i = 0; i < (int)(PAGE_SIZE / sizeof(unsigned long)); i++)
        {
            a[i] = 0x1111111111111111UL;
            b[i] = 0x2222222222222222UL;
        }
        int a_ok = 1, b_ok = 1;
        for (int i = 0; i < (int)(PAGE_SIZE / sizeof(unsigned long)); i++)
        {
            if (a[i] != 0x1111111111111111UL)
                a_ok = 0;
            if (b[i] != 0x2222222222222222UL)
                b_ok = 0;
        }
        TEST_ASSERT("page a isolated", a_ok);
        TEST_ASSERT("page b isolated", b_ok);
        pmm_free_page(a);
        pmm_free_page(b);
    }
    TEST_PASS("data isolation");

    // multi-page blocks don't corrupt each other
    {
        unsigned char* a = (unsigned char*)pmm_alloc_pages(4);
        unsigned char* b = (unsigned char*)pmm_alloc_pages(4);
        memset(a, 0xAA, 4 * PAGE_SIZE);
        memset(b, 0xBB, 4 * PAGE_SIZE);
        // Spot-check pages
        int ok = 1;
        for (int pg = 0; pg < 4; pg++)
        {
            if (a[pg * PAGE_SIZE] != 0xAA)
                ok = 0;
            if (a[pg * PAGE_SIZE + PAGE_SIZE - 1] != 0xAA)
                ok = 0;
            if (b[pg * PAGE_SIZE] != 0xBB)
                ok = 0;
            if (b[pg * PAGE_SIZE + PAGE_SIZE - 1] != 0xBB)
                ok = 0;
        }
        TEST_ASSERT("multi-page isolation", ok);
        pmm_free_pages(a, 4);
        pmm_free_pages(b, 4);
    }
    TEST_PASS("multi-page isolation");

    // mixed single + multi-page

    // interleaved single and multi-page allocs
    {
        void* s1 = pmm_alloc_page();
        void* m1 = pmm_alloc_pages(4);
        void* s2 = pmm_alloc_page();
        void* m2 = pmm_alloc_pages(8);
        void* s3 = pmm_alloc_page();
        void* m3 = pmm_alloc_pages(2);

        TEST_ASSERT("mixed s1", s1 != NULL);
        TEST_ASSERT("mixed m1", m1 != NULL);
        TEST_ASSERT("mixed s2", s2 != NULL);
        TEST_ASSERT("mixed m2", m2 != NULL);
        TEST_ASSERT("mixed s3", s3 != NULL);
        TEST_ASSERT("mixed m3", m3 != NULL);

        // Free in scrambled order
        pmm_free_pages(m2, 8);
        pmm_free_page(s1);
        pmm_free_pages(m3, 2);
        pmm_free_page(s3);
        pmm_free_pages(m1, 4);
        pmm_free_page(s2);
    }
    TEST_PASS("mixed single/multi interleaved");

    // alloc multi, free, alloc singles from it
    {
        void* blk = pmm_alloc_pages(4);
        pmm_free_pages(blk, 4);
        // Should be able to alloc 4 individual pages now
        void* pages[4];
        for (int i = 0; i < 4; i++)
        {
            pages[i] = pmm_alloc_page();
            TEST_ASSERT("split-multi alloc", pages[i] != NULL);
        }
        for (int i = 0; i < 4; i++)
            pmm_free_page(pages[i]);
    }
    TEST_PASS("multi→single split");

    // fragmentation patterns

    // alternating free pattern
    {
        void* pages[16];
        for (int i = 0; i < 16; i++)
            pages[i] = pmm_alloc_page();
        // Free even indices
        for (int i = 0; i < 16; i += 2)
            pmm_free_page(pages[i]);
        // Re-alloc into freed slots
        for (int i = 0; i < 16; i += 2)
        {
            pages[i] = pmm_alloc_page();
            TEST_ASSERT("frag re-alloc", pages[i] != NULL);
        }
        for (int i = 0; i < 16; i++)
            pmm_free_page(pages[i]);
    }
    TEST_PASS("alternating frag");

    // swiss-cheese: free scattered pages
    {
        void* pages[20];
        for (int i = 0; i < 20; i++)
            pages[i] = pmm_alloc_page();
        // Free a scattered pattern
        int free_idx[] = {1, 3, 7, 8, 12, 15, 18};
        for (int i = 0; i < 7; i++)
            pmm_free_page(pages[free_idx[i]]);
        // Re-alloc the freed ones
        for (int i = 0; i < 7; i++)
        {
            pages[free_idx[i]] = pmm_alloc_page();
            TEST_ASSERT("swiss realloc", pages[free_idx[i]] != NULL);
        }
        for (int i = 0; i < 20; i++)
            pmm_free_page(pages[i]);
    }
    TEST_PASS("swiss-cheese frag");

    // fragment then request larger block
    //    Fragment the allocator, then request a multi-page block to
    //    verify buddy merging still works in a fragmented state.
    {
        void* pages[8];
        for (int i = 0; i < 8; i++)
            pages[i] = pmm_alloc_page();
        // Free all → buddies should merge back up
        for (int i = 0; i < 8; i++)
            pmm_free_page(pages[i]);
        // Now alloc 8-page block — requires full merge to order 3
        void* big = pmm_alloc_pages(8);
        TEST_ASSERT("post-frag 8p alloc", big != NULL);
        pmm_free_pages(big, 8);
    }
    TEST_PASS("frag then large alloc");

    // stress tests

    // alloc 128 pages, free all
    {
        void* pages[128];
        for (int i = 0; i < 128; i++)
        {
            pages[i] = pmm_alloc_page();
            TEST_ASSERT("128x alloc", pages[i] != NULL);
        }
        for (int i = 0; i < 128; i++)
            pmm_free_page(pages[i]);
        // Allocator should recover
        void* p = pmm_alloc_page();
        TEST_ASSERT("post-128 alloc ok", p != NULL);
        pmm_free_page(p);
    }
    TEST_PASS("stress 128 pages");

    // sawtooth: batch alloc/free ×5
    {
        for (int round = 0; round < 5; round++)
        {
            void* pages[32];
            for (int i = 0; i < 32; i++)
            {
                pages[i] = pmm_alloc_page();
                TEST_ASSERT("sawtooth alloc", pages[i] != NULL);
            }
            for (int i = 0; i < 32; i++)
                pmm_free_page(pages[i]);
        }
    }
    TEST_PASS("sawtooth 5 rounds");

    // wave pattern: alloc 4, free 2, repeat
    {
        void* pages[40];
        int count = 0;
        for (int wave = 0; wave < 10; wave++)
        {
            for (int i = 0; i < 4; i++)
            {
                pages[count] = pmm_alloc_page();
                TEST_ASSERT("wave alloc", pages[count] != NULL);
                count++;
            }
            for (int i = 0; i < 2 && count > 0; i++)
            {
                count--;
                pmm_free_page(pages[count]);
            }
        }
        for (int i = count - 1; i >= 0; i--)
            pmm_free_page(pages[i]);
    }
    TEST_PASS("wave pattern");

    // multi-page stress: alloc/free 4p blocks ×16
    {
        void* blocks[16];
        for (int i = 0; i < 16; i++)
        {
            blocks[i] = pmm_alloc_pages(4);
            TEST_ASSERT("4p stress alloc", blocks[i] != NULL);
        }
        // Free in reverse order
        for (int i = 15; i >= 0; i--)
            pmm_free_pages(blocks[i], 4);
    }
    TEST_PASS("multi-page stress 16x4p");

    // free-order independence

    // fifo free
    {
        void* a = pmm_alloc_page();
        void* b = pmm_alloc_page();
        void* c = pmm_alloc_page();
        void* d = pmm_alloc_page();
        pmm_free_page(a);
        pmm_free_page(b);
        pmm_free_page(c);
        pmm_free_page(d);
    }
    TEST_PASS("FIFO free");

    // lifo free
    {
        void* a = pmm_alloc_page();
        void* b = pmm_alloc_page();
        void* c = pmm_alloc_page();
        void* d = pmm_alloc_page();
        pmm_free_page(d);
        pmm_free_page(c);
        pmm_free_page(b);
        pmm_free_page(a);
    }
    TEST_PASS("LIFO free");

    // scrambled free
    {
        void* a = pmm_alloc_page();
        void* b = pmm_alloc_page();
        void* c = pmm_alloc_page();
        void* d = pmm_alloc_page();
        pmm_free_page(c);
        pmm_free_page(a);
        pmm_free_page(d);
        pmm_free_page(b);
    }
    TEST_PASS("scrambled free");

    // full-page write verification

    // write full 4kb page
    {
        unsigned char* p = (unsigned char*)pmm_alloc_page();
        TEST_ASSERT("fullpage alloc", p != NULL);
        for (int i = 0; i < PAGE_SIZE; i++)
            p[i] = (unsigned char)(i & 0xFF);
        int ok = 1;
        for (int i = 0; i < PAGE_SIZE; i++)
            if (p[i] != (unsigned char)(i & 0xFF))
            {
                ok = 0;
                break;
            }
        TEST_ASSERT("fullpage pattern", ok);
        pmm_free_page(p);
    }
    TEST_PASS("full 4KB write");

    // write boundary words of each page in multi-page block
    {
        unsigned long count = 16;
        unsigned long* blk = (unsigned long*)pmm_alloc_pages(count);
        TEST_ASSERT("16p word alloc", blk != NULL);
        unsigned long words_per_page = PAGE_SIZE / sizeof(unsigned long);
        for (unsigned long pg = 0; pg < count; pg++)
        {
            // Write first and last word of each page
            blk[pg * words_per_page] = 0xAAAAAAAA00000000UL | pg;
            blk[pg * words_per_page + words_per_page - 1] = 0xBBBBBBBB00000000UL | pg;
        }
        int ok = 1;
        for (unsigned long pg = 0; pg < count; pg++)
        {
            if (blk[pg * words_per_page] != (0xAAAAAAAA00000000UL | pg))
                ok = 0;
            if (blk[pg * words_per_page + words_per_page - 1] != (0xBBBBBBBB00000000UL | pg))
                ok = 0;
        }
        TEST_ASSERT("16p boundary words ok", ok);
        pmm_free_pages(blk, count);
    }
    TEST_PASS("16-page boundary words");

    // growing & shrinking allocation sizes

    // growing: 1p → 2p → 4p → 8p → 16p → 32p
    {
        void* ptrs[6];
        for (int i = 0; i < 6; i++)
        {
            unsigned long count = 1UL << i;
            ptrs[i] = pmm_alloc_pages(count);
            TEST_ASSERT("growing alloc", ptrs[i] != NULL);
        }
        for (int i = 5; i >= 0; i--)
            pmm_free_pages(ptrs[i], 1UL << i);
    }
    TEST_PASS("growing alloc sizes");

    // shrinking: 32p → 16p → 8p → 4p → 2p → 1p
    {
        void* ptrs[6];
        for (int i = 0; i < 6; i++)
        {
            unsigned long count = 32UL >> i;
            ptrs[i] = pmm_alloc_pages(count);
            TEST_ASSERT("shrinking alloc", ptrs[i] != NULL);
        }
        for (int i = 0; i < 6; i++)
            pmm_free_pages(ptrs[i], 32UL >> i);
    }
    TEST_PASS("shrinking alloc sizes");

    // lifecycle

    // multi-page lifecycle: alloc → write → free → re-alloc
    {
        unsigned char* blk = (unsigned char*)pmm_alloc_pages(4);
        for (int i = 0; i < 4; i++)
            blk[i * PAGE_SIZE] = (unsigned char)(0xF0 | i);
        pmm_free_pages(blk, 4);
        unsigned char* blk2 = (unsigned char*)pmm_alloc_pages(4);
        TEST_ASSERT("lifecycle re-alloc", blk2 != NULL);
        // Write new pattern
        for (int i = 0; i < 4; i++)
            blk2[i * PAGE_SIZE] = (unsigned char)(0xA0 | i);
        int ok = 1;
        for (int i = 0; i < 4; i++)
            if (blk2[i * PAGE_SIZE] != (unsigned char)(0xA0 | i))
                ok = 0;
        TEST_ASSERT("lifecycle new data", ok);
        pmm_free_pages(blk2, 4);
    }
    TEST_PASS("multi-page lifecycle");

    // complex multi-order lifecycle
    {
        // Phase 1: mixed allocs
        void* p1 = pmm_alloc_page();
        void* p4 = pmm_alloc_pages(4);
        void* p2 = pmm_alloc_pages(2);
        void* p8 = pmm_alloc_pages(8);

        // Phase 2: free middle ones
        pmm_free_pages(p4, 4);
        pmm_free_pages(p2, 2);

        // Phase 3: alloc different sizes from freed space
        void* q2 = pmm_alloc_pages(2);
        void* q1 = pmm_alloc_page();
        TEST_ASSERT("lifecycle q2", q2 != NULL);
        TEST_ASSERT("lifecycle q1", q1 != NULL);

        // Phase 4: cleanup
        pmm_free_page(p1);
        pmm_free_pages(p8, 8);
        pmm_free_pages(q2, 2);
        pmm_free_page(q1);

        // Phase 5: large alloc should work (everything merged)
        void* big = pmm_alloc_pages(16);
        TEST_ASSERT("lifecycle big alloc", big != NULL);
        pmm_free_pages(big, 16);
    }
    TEST_PASS("complex multi-order lifecycle");

    TEST_SUITE_END("Physical Memory Manager");
}
