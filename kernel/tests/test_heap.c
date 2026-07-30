#include "test.h"
#include "mm/heap.h"
#include "string.h"
#include "types.h"

#define TEST_HEADER_SIZE 32   /* sizeof(block_header) */
#define LARGE            2048 /* > SLAB_MAX (1024), forces first-fit path */

void test_heap(void)
{
    TEST_SUITE_BEGIN("Heap Allocator");

    // basic functionality

    // basic alloc / free
    {
        void *p = heap_malloc(64);
        TEST_ASSERT("heap_malloc returns non-null", p != NULL);
        TEST_ASSERT("heap_malloc 16-byte aligned", ((unsigned long)p & 0xF) == 0);
        heap_free(p);
    }
    TEST_PASS("basic alloc/free");

    // zero-size returns null
    TEST_ASSERT("zero alloc returns null", heap_malloc(0) == NULL);
    TEST_PASS("zero-size returns NULL");

    // excessively large alloc should return null
    TEST_ASSERT("huge alloc returns null", heap_malloc(0xFFFFFFFFFFFFFFFFULL) == NULL);
    TEST_PASS("huge alloc returns NULL");

    // heap_free(null) is safe
    heap_free(NULL);
    TEST_PASS("heap_free(NULL) safe");

    // 1-byte allocation
    {
        void *p = heap_malloc(1);
        TEST_ASSERT("1-byte alloc non-null", p != NULL);
        TEST_ASSERT("1-byte aligned", ((unsigned long)p & 0xF) == 0);
        *(unsigned char *)p = 0x42;
        TEST_ASSERT("1-byte write", *(unsigned char *)p == 0x42);
        heap_free(p);
    }
    TEST_PASS("1-byte alloc");

    // alignment guarantees

    // alignment across many sizes including odd/edge values
    {
        unsigned long sizes[] = {1,   2,   3,   4,   5,   7,   8,   9,    13,   15,
                                 16,  17,  23,  31,  32,  33,  48,  63,   64,   65,
                                 100, 127, 128, 255, 256, 500, 512, 1000, 1024, 4096};
        int nsizes = sizeof(sizes) / sizeof(sizes[0]);
        void *ptrs[30];
        int all_aligned = 1;
        for (int i = 0; i < nsizes; i++) {
            ptrs[i] = heap_malloc(sizes[i]);
            TEST_ASSERT("align alloc non-null", ptrs[i] != NULL);
            if (((unsigned long)ptrs[i] & 0xF) != 0)
                all_aligned = 0;
        }
        TEST_ASSERT("all pointers 16-byte aligned", all_aligned);
        for (int i = nsizes - 1; i >= 0; i--)
            heap_free(ptrs[i]);
    }
    TEST_PASS("alignment 30 sizes");

    // alignment after free-and-realloc cycle
    {
        for (int i = 1; i <= 20; i++) {
            void *p = heap_malloc(i);
            TEST_ASSERT("cycle aligned", ((unsigned long)p & 0xF) == 0);
            heap_free(p);
        }
    }
    TEST_PASS("alignment after reuse");

    // non-overlapping allocations

    // two allocs don't overlap
    {
        void *a = heap_malloc(128);
        void *b = heap_malloc(128);
        TEST_ASSERT("a non-null", a != NULL);
        TEST_ASSERT("b non-null", b != NULL);
        unsigned long dist = (unsigned long)b > (unsigned long)a
                                 ? (unsigned long)b - (unsigned long)a
                                 : (unsigned long)a - (unsigned long)b;
        TEST_ASSERT("no overlap (dist >= 128)", dist >= 128);
        heap_free(a);
        heap_free(b);
    }
    TEST_PASS("two allocs no overlap");

    // many small allocs all distinct
    {
        void *ptrs[64];
        for (int i = 0; i < 64; i++) {
            ptrs[i] = heap_malloc(32);
            TEST_ASSERT("64x alloc", ptrs[i] != NULL);
        }
        int distinct = 1;
        for (int i = 0; i < 64 && distinct; i++)
            for (int j = i + 1; j < 64 && distinct; j++)
                if (ptrs[i] == ptrs[j])
                    distinct = 0;
        TEST_ASSERT("all 64 distinct", distinct);
        for (int i = 0; i < 64; i++)
            heap_free(ptrs[i]);
    }
    TEST_PASS("64 allocs distinct");

    // adjacent alloc data isolation
    {
        unsigned char *a = (unsigned char *)heap_malloc(64);
        unsigned char *b = (unsigned char *)heap_malloc(64);
        memset(a, 0xAA, 64);
        memset(b, 0xBB, 64);
        int a_ok = 1, b_ok = 1;
        for (int i = 0; i < 64; i++) {
            if (a[i] != 0xAA)
                a_ok = 0;
            if (b[i] != 0xBB)
                b_ok = 0;
        }
        TEST_ASSERT("block a intact", a_ok);
        TEST_ASSERT("block b intact", b_ok);
        heap_free(a);
        heap_free(b);
    }
    TEST_PASS("data isolation");

    // memory reuse

    // freed block is reused for same-size alloc
    {
        void *p1 = heap_malloc(64);
        heap_free(p1);
        void *p2 = heap_malloc(64);
        TEST_ASSERT("reuses freed block", p2 == p1);
        heap_free(p2);
    }
    TEST_PASS("reuse same size");

    // freed block reused for smaller alloc (first-fit path, sizes > SLAB_MAX)
    {
        void *p1 = heap_malloc(4096);
        heap_free(p1);
        void *p2 = heap_malloc(LARGE);
        // should reuse same address (first-fit), possibly splitting
        TEST_ASSERT("reuses for smaller", p2 == p1);
        heap_free(p2);
    }
    TEST_PASS("reuse smaller alloc");

    // first-fit: earlier free block chosen over later (sizes > SLAB_MAX)
    {
        void *a = heap_malloc(LARGE);
        void *b = heap_malloc(LARGE);
        void *c = heap_malloc(LARGE);
        heap_free(a);
        heap_free(c);
        void *d = heap_malloc(LARGE);
        // first-fit should pick 'a' position
        TEST_ASSERT("first-fit picks earlier", d == a);
        heap_free(d);
        heap_free(b);
    }
    TEST_PASS("first-fit ordering");

    /*
     * The tests from here to the end of the coalescing group assert *where* a
     * block lands, which only holds if no earlier hole can serve the request.
     * Consume every hole big enough to interfere and hold them for the whole
     * group; the heap having to grow is the signal that none is left. Draining
     * a fixed number of bytes instead would depend on how much heap the kernel
     * happened to use before the tests ran.
     */
    void *layout_fill[64];
    int layout_nfill = 0;
    {
        unsigned long total_before = heap_get_total();
        while (layout_nfill < 64 && heap_get_total() == total_before) {
            void *f = heap_malloc(LARGE);
            if (!f) {
                break;
            }
            layout_fill[layout_nfill++] = f;
        }
    }

    // block splitting (sizes > SLAB_MAX to exercise first-fit path)

    // split occurs when remainder >= header_size + 16 (48)
    //    Allocate a big block, free it, then allocate much smaller.
    //    The remainder should be split into a second usable block.
    {
        void *big = heap_malloc(8192);
        heap_free(big);
        // Freed block has size >= 8192.
        // Allocate LARGE from it. Remainder = 8192-2048-32 = 6112 >= 48 -> splits.
        void *small = heap_malloc(LARGE);
        TEST_ASSERT("split: first part", small == big);
        // Second allocation should come from the split remainder
        void *next = heap_malloc(LARGE);
        TEST_ASSERT("split: second from remainder", next != NULL);
        // next should be right after small's block: small + LARGE + HEADER_SIZE + FOOTER_SIZE (16)
        unsigned long expected = (unsigned long)small + LARGE + TEST_HEADER_SIZE + 16;
        TEST_ASSERT("split: contiguous layout", (unsigned long)next == expected);
        heap_free(small);
        heap_free(next);
    }
    TEST_PASS("block splitting basic");

    // no split when remainder < header_size + 16 (48)
    //    If the remaining space after alloc is < 48 bytes, no split occurs.
    {
        // Allocate then free a block. Re-alloc the same size -> no split expected.
        void *p = heap_malloc(LARGE);
        heap_free(p);
        void *q = heap_malloc(LARGE);
        TEST_ASSERT("no-split same size reuse", q == p);
        heap_free(q);
    }
    TEST_PASS("no unnecessary split");

    // split creates usable blocks (write to both halves, sizes > SLAB_MAX)
    {
        void *big = heap_malloc(8192);
        heap_free(big);
        unsigned char *a = (unsigned char *)heap_malloc(LARGE);
        unsigned char *b = (unsigned char *)heap_malloc(LARGE);
        // Write full patterns to both split parts
        memset(a, 0x11, LARGE);
        memset(b, 0x22, LARGE);
        int a_ok = 1, b_ok = 1;
        for (int i = 0; i < LARGE; i++) {
            if (a[i] != 0x11)
                a_ok = 0;
            if (b[i] != 0x22)
                b_ok = 0;
        }
        TEST_ASSERT("split-a data intact", a_ok);
        TEST_ASSERT("split-b data intact", b_ok);
        heap_free(a);
        heap_free(b);
    }
    TEST_PASS("split blocks usable");

    // repeated splitting exhausts a block correctly (sizes > SLAB_MAX)
    {
        void *big = heap_malloc(8192);
        heap_free(big);
        // Each LARGE (2048) alloc uses 2048 + 32 = 2080 bytes.
        // From 8192 bytes, we can fit floor(8192 / 2080) = 3 splits.
        // (3 * 2080 = 6240, remainder 1952 < 2080+32 so 4th needs expansion)
        void *ptrs[3];
        int count = 0;
        for (int i = 0; i < 3; i++) {
            ptrs[i] = heap_malloc(LARGE);
            if (ptrs[i] != NULL)
                count++;
        }
        TEST_ASSERT("repeated split fills block", count == 3);
        for (int i = 0; i < 3; i++)
            heap_free(ptrs[i]);
    }
    TEST_PASS("repeated splitting");

    // coalescing (sizes > SLAB_MAX)

    // two adjacent free blocks coalesce
    {
        void *a = heap_malloc(LARGE);
        void *b = heap_malloc(LARGE);
        void *guard = heap_malloc(LARGE); // prevent merging beyond b
        heap_free(a);
        heap_free(b);
        // a+b should coalesce: 2048 + 32(header) + 2048 = 4128 usable
        void *merged = heap_malloc(4128);
        TEST_ASSERT("coalesce: merged alloc succeeds", merged != NULL);
        TEST_ASSERT("coalesce: merged at a's position", merged == a);
        heap_free(merged);
        heap_free(guard);
    }
    TEST_PASS("two-block coalesce");

    // three adjacent free blocks coalesce (chain)
    {
        void *a = heap_malloc(LARGE);
        void *b = heap_malloc(LARGE);
        void *c = heap_malloc(LARGE);
        void *guard = heap_malloc(LARGE);
        heap_free(a);
        heap_free(b);
        heap_free(c);
        // Total coalesced usable = 2048 + 32 + 2048 + 32 + 2048 = 6208
        void *merged = heap_malloc(6208);
        TEST_ASSERT("3-coalesce succeeds", merged != NULL);
        TEST_ASSERT("3-coalesce at a's position", merged == a);
        heap_free(merged);
        heap_free(guard);
    }
    TEST_PASS("three-block coalesce");

    // non-adjacent free blocks do not coalesce
    {
        void *a = heap_malloc(LARGE);
        void *b = heap_malloc(LARGE); // stays allocated
        void *c = heap_malloc(LARGE);
        heap_free(a);
        heap_free(c);
        // a and c are free but b separates them: should NOT coalesce
        // Trying to alloc 4128 should NOT reuse a (a is only 2048)
        void *big = heap_malloc(4128);
        TEST_ASSERT("no false coalesce", big != a);
        heap_free(big);
        heap_free(b);
    }
    TEST_PASS("non-adjacent no coalesce");

    // coalesce then split: free two adjacent, alloc smaller
    {
        void *a = heap_malloc(LARGE);
        void *b = heap_malloc(LARGE);
        void *guard = heap_malloc(LARGE);
        heap_free(a);
        heap_free(b);
        // Coalesced block = 4128, alloc LARGE from it -> split (4128-2048-32=2048 >= 48)
        void *small = heap_malloc(LARGE);
        TEST_ASSERT("coalesce+split: first part", small == a);
        void *next = heap_malloc(LARGE);
        TEST_ASSERT("coalesce+split: second from remainder", next != NULL);
        heap_free(small);
        heap_free(next);
        heap_free(guard);
    }
    TEST_PASS("coalesce then split");

    // free in reverse order still coalesces
    {
        void *a = heap_malloc(LARGE);
        void *b = heap_malloc(LARGE);
        void *guard = heap_malloc(LARGE);
        // Free in forward order vs reverse - coalescing walks the list
        heap_free(b);
        heap_free(a);
        void *merged = heap_malloc(4128);
        TEST_ASSERT("reverse free coalesces", merged != NULL);
        heap_free(merged);
        heap_free(guard);
    }
    TEST_PASS("reverse-order coalesce");

    for (int i = layout_nfill - 1; i >= 0; i--) {
        heap_free(layout_fill[i]);
    }

    // heap expansion

    // allocation larger than initial page triggers expansion
    {
        void *big = heap_malloc(8192); // 2 pages
        TEST_ASSERT("expand: large alloc ok", big != NULL);
        TEST_ASSERT("expand: aligned", ((unsigned long)big & 0xF) == 0);
        // Write boundaries
        *(volatile unsigned char *)big = 0xDE;
        *((volatile unsigned char *)big + 8191) = 0xAD;
        TEST_ASSERT("expand: first byte", *(volatile unsigned char *)big == 0xDE);
        TEST_ASSERT("expand: last byte", *((volatile unsigned char *)big + 8191) == 0xAD);
        heap_free(big);
    }
    TEST_PASS("heap expansion 8KB");

    // large allocation (1 mb)
    {
        void *big = heap_malloc(1024 * 1024);
        TEST_ASSERT("1MB alloc non-null", big != NULL);
        TEST_ASSERT("1MB aligned", ((unsigned long)big & 0xF) == 0);
        // Touch first, middle, last
        *(volatile unsigned char *)big = 0xAB;
        *((volatile unsigned char *)big + 512 * 1024) = 0xCD;
        *((volatile unsigned char *)big + 1024 * 1024 - 1) = 0xEF;
        TEST_ASSERT("1MB first", *(volatile unsigned char *)big == 0xAB);
        TEST_ASSERT("1MB middle", *((volatile unsigned char *)big + 512 * 1024) == 0xCD);
        TEST_ASSERT("1MB last", *((volatile unsigned char *)big + 1024 * 1024 - 1) == 0xEF);
        heap_free(big);
    }
    TEST_PASS("heap expansion 1MB");

    // multiple expansions
    {
        void *a = heap_malloc(16384);
        void *b = heap_malloc(16384);
        void *c = heap_malloc(16384);
        TEST_ASSERT("multi-expand a", a != NULL);
        TEST_ASSERT("multi-expand b", b != NULL);
        TEST_ASSERT("multi-expand c", c != NULL);
        heap_free(b);
        heap_free(a);
        heap_free(c);
    }
    TEST_PASS("multiple expansions");

    // page-aligned allocation sizes
    {
        // Allocate exactly 4096 (PAGE_SIZE) bytes
        void *page = heap_malloc(4096);
        TEST_ASSERT("page-size alloc ok", page != NULL);
        memset(page, 0xFF, 4096);
        unsigned char *cp = (unsigned char *)page;
        int ok = 1;
        for (int i = 0; i < 4096; i++)
            if (cp[i] != 0xFF) {
                ok = 0;
                break;
            }
        TEST_ASSERT("page-size canary", ok);
        heap_free(page);
    }
    TEST_PASS("page-aligned alloc");

    // write pattern & data integrity

    // sequential byte pattern
    {
        unsigned char *mem = (unsigned char *)heap_malloc(256);
        TEST_ASSERT("pattern alloc", mem != NULL);
        for (int i = 0; i < 256; i++)
            mem[i] = (unsigned char)i;
        int ok = 1;
        for (int i = 0; i < 256; i++)
            if (mem[i] != (unsigned char)i) {
                ok = 0;
                break;
            }
        TEST_ASSERT("sequential pattern", ok);
        heap_free(mem);
    }
    TEST_PASS("sequential byte pattern");

    // boundary writes to exact allocation size
    //    Write every byte up to the requested size, making sure we don't
    //    corrupt the header of the next block.
    {
        unsigned long sizes[] = {1, 15, 16, 17, 31, 32, 33, 47, 48, 49, 63, 64};
        int nsizes = sizeof(sizes) / sizeof(sizes[0]);
        for (int s = 0; s < nsizes; s++) {
            unsigned char *p = (unsigned char *)heap_malloc(sizes[s]);
            TEST_ASSERT("boundary alloc", p != NULL);
            // Fill entire requested region
            for (unsigned long i = 0; i < sizes[s]; i++)
                p[i] = (unsigned char)(i ^ 0xAA);
            int ok = 1;
            for (unsigned long i = 0; i < sizes[s]; i++)
                if (p[i] != (unsigned char)(i ^ 0xAA)) {
                    ok = 0;
                    break;
                }
            TEST_ASSERT("boundary verify", ok);
            heap_free(p);
        }
    }
    TEST_PASS("boundary writes");

    // alloc does not destroy neighbor data
    {
        unsigned char *a = (unsigned char *)heap_malloc(128);
        unsigned char *b = (unsigned char *)heap_malloc(128);
        unsigned char *c = (unsigned char *)heap_malloc(128);
        memset(a, 0x11, 128);
        memset(b, 0x22, 128);
        memset(c, 0x33, 128);
        // Verify none corrupted each other
        int ok = 1;
        for (int i = 0; i < 128; i++) {
            if (a[i] != 0x11)
                ok = 0;
            if (b[i] != 0x22)
                ok = 0;
            if (c[i] != 0x33)
                ok = 0;
        }
        TEST_ASSERT("triple isolation", ok);
        heap_free(a);
        heap_free(b);
        heap_free(c);
    }
    TEST_PASS("neighbor data isolation");

    // unsigned long pattern (word-aligned writes)
    {
        int nwords = 128;
        unsigned long *arr = (unsigned long *)heap_malloc(nwords * sizeof(unsigned long));
        TEST_ASSERT("word alloc", arr != NULL);
        for (int i = 0; i < nwords; i++)
            arr[i] = 0xDEADBEEF00000000UL | (unsigned long)i;
        int ok = 1;
        for (int i = 0; i < nwords; i++)
            if (arr[i] != (0xDEADBEEF00000000UL | (unsigned long)i)) {
                ok = 0;
                break;
            }
        TEST_ASSERT("word pattern intact", ok);
        heap_free(arr);
    }
    TEST_PASS("word-aligned pattern");

    // free-order independence

    // fifo free
    {
        void *a = heap_malloc(48);
        void *b = heap_malloc(48);
        void *c = heap_malloc(48);
        heap_free(a);
        heap_free(b);
        heap_free(c);
    }
    TEST_PASS("FIFO free");

    // lifo free
    {
        void *a = heap_malloc(48);
        void *b = heap_malloc(48);
        void *c = heap_malloc(48);
        heap_free(c);
        heap_free(b);
        heap_free(a);
    }
    TEST_PASS("LIFO free");

    // middle-first free
    {
        void *a = heap_malloc(48);
        void *b = heap_malloc(48);
        void *c = heap_malloc(48);
        heap_free(b);
        heap_free(a);
        heap_free(c);
    }
    TEST_PASS("middle-first free");

    // fragmentation patterns

    // alternating holes
    {
        void *ptrs[16];
        for (int i = 0; i < 16; i++)
            ptrs[i] = heap_malloc(64);
        // Free even-indexed blocks -> creates 8 "holes"
        for (int i = 0; i < 16; i += 2)
            heap_free(ptrs[i]);
        // Reallocate into holes
        for (int i = 0; i < 16; i += 2) {
            ptrs[i] = heap_malloc(64);
            TEST_ASSERT("frag realloc", ptrs[i] != NULL);
        }
        for (int i = 0; i < 16; i++)
            heap_free(ptrs[i]);
    }
    TEST_PASS("alternating holes");

    // every-third pattern
    {
        void *ptrs[12];
        for (int i = 0; i < 12; i++)
            ptrs[i] = heap_malloc(32);
        // Free every 3rd
        for (int i = 0; i < 12; i += 3)
            heap_free(ptrs[i]);
        // Allocate into freed slots
        for (int i = 0; i < 12; i += 3) {
            ptrs[i] = heap_malloc(32);
            TEST_ASSERT("every-3rd realloc", ptrs[i] != NULL);
        }
        for (int i = 0; i < 12; i++)
            heap_free(ptrs[i]);
    }
    TEST_PASS("every-third pattern");

    // swiss cheese: random-like free pattern
    {
        void *ptrs[20];
        for (int i = 0; i < 20; i++)
            ptrs[i] = heap_malloc(48);
        // Free a scattered pattern: 1,4,6,9,11,14,16,19
        int free_idx[] = {1, 4, 6, 9, 11, 14, 16, 19};
        for (int i = 0; i < 8; i++)
            heap_free(ptrs[free_idx[i]]);
        // Re-alloc same size into freed holes
        for (int i = 0; i < 8; i++) {
            ptrs[free_idx[i]] = heap_malloc(48);
            TEST_ASSERT("swiss cheese realloc", ptrs[free_idx[i]] != NULL);
        }
        for (int i = 0; i < 20; i++)
            heap_free(ptrs[i]);
    }
    TEST_PASS("swiss cheese pattern");

    // mixed size allocations

    // small + medium + large interleaved
    {
        void *s1 = heap_malloc(16);
        void *m1 = heap_malloc(256);
        void *l1 = heap_malloc(4096);
        void *s2 = heap_malloc(32);
        void *m2 = heap_malloc(512);
        TEST_ASSERT("mixed s1", s1 != NULL);
        TEST_ASSERT("mixed m1", m1 != NULL);
        TEST_ASSERT("mixed l1", l1 != NULL);
        TEST_ASSERT("mixed s2", s2 != NULL);
        TEST_ASSERT("mixed m2", m2 != NULL);
        heap_free(m1);
        heap_free(s2);
        heap_free(l1);
        heap_free(s1);
        heap_free(m2);
    }
    TEST_PASS("mixed sizes interleaved");

    // growing allocations: 16 -> 32 -> 64 -> ... -> 4096
    {
        void *ptrs[9];
        for (int i = 0; i < 9; i++) {
            unsigned long sz = 16UL << i; // 16,32,64,...,4096
            ptrs[i] = heap_malloc(sz);
            TEST_ASSERT("growing alloc", ptrs[i] != NULL);
        }
        for (int i = 8; i >= 0; i--)
            heap_free(ptrs[i]);
    }
    TEST_PASS("growing allocs");

    // shrinking allocations: 4096 -> 2048 -> ... -> 16
    {
        void *ptrs[9];
        for (int i = 0; i < 9; i++) {
            unsigned long sz = 4096UL >> i; // 4096,2048,...,16
            ptrs[i] = heap_malloc(sz);
            TEST_ASSERT("shrinking alloc", ptrs[i] != NULL);
        }
        for (int i = 0; i < 9; i++)
            heap_free(ptrs[i]);
    }
    TEST_PASS("shrinking allocs");

    // power-of-2 allocations with canary
    {
        unsigned long po2_sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
        int n = sizeof(po2_sizes) / sizeof(po2_sizes[0]);
        for (int i = 0; i < n; i++) {
            void *ptr = heap_malloc(po2_sizes[i]);
            TEST_ASSERT("po2 alloc", ptr != NULL);
            TEST_ASSERT("po2 aligned", ((unsigned long)ptr & 0xF) == 0);
            memset(ptr, 0xCC, po2_sizes[i]);
            int ok = 1;
            unsigned char *cp = (unsigned char *)ptr;
            for (unsigned long j = 0; j < po2_sizes[i]; j++)
                if (cp[j] != 0xCC) {
                    ok = 0;
                    break;
                }
            TEST_ASSERT("po2 canary", ok);
            heap_free(ptr);
        }
    }
    TEST_PASS("power-of-2 canary");

    // stress tests

    // rapid alloc/free same size ×200
    {
        for (int i = 0; i < 200; i++) {
            void *ptr = heap_malloc(128);
            TEST_ASSERT("rapid alloc", ptr != NULL);
            *(volatile unsigned long *)ptr = 0xCAFEBABEUL;
            TEST_ASSERT("rapid canary", *(volatile unsigned long *)ptr == 0xCAFEBABEUL);
            heap_free(ptr);
        }
    }
    TEST_PASS("rapid cycle x200");

    // fill-and-drain 128 blocks
    {
        void *ptrs[128];
        for (int i = 0; i < 128; i++) {
            ptrs[i] = heap_malloc(64);
            TEST_ASSERT("fill alloc", ptrs[i] != NULL);
        }
        for (int i = 127; i >= 0; i--)
            heap_free(ptrs[i]);

        // After draining, allocator should still work normally
        void *p = heap_malloc(64);
        TEST_ASSERT("post-drain alloc", p != NULL);
        heap_free(p);
    }
    TEST_PASS("fill-and-drain 128");

    // sawtooth: alloc batch then free batch, repeat
    {
        for (int round = 0; round < 5; round++) {
            void *ptrs[32];
            for (int i = 0; i < 32; i++) {
                ptrs[i] = heap_malloc(48);
                TEST_ASSERT("sawtooth alloc", ptrs[i] != NULL);
            }
            for (int i = 0; i < 32; i++)
                heap_free(ptrs[i]);
        }
    }
    TEST_PASS("sawtooth 5 rounds");

    // interleaved alloc/free (wave pattern)
    {
        void *ptrs[32];
        int count = 0;
        // Allocate 4, free 2, allocate 4, free 2, ...
        for (int wave = 0; wave < 8; wave++) {
            for (int i = 0; i < 4; i++) {
                ptrs[count] = heap_malloc(64);
                TEST_ASSERT("wave alloc", ptrs[count] != NULL);
                count++;
            }
            for (int i = 0; i < 2 && count > 0; i++) {
                count--;
                heap_free(ptrs[count]);
            }
        }
        // Free remaining
        for (int i = count - 1; i >= 0; i--)
            heap_free(ptrs[i]);
    }
    TEST_PASS("wave alloc/free");

    // header integrity after operations

    // write full requested size, then free and re-alloc
    //    If writing up to the boundary corrupted the next header, the
    //    subsequent alloc/free would crash or return bad data.
    {
        unsigned char *a = (unsigned char *)heap_malloc(48);
        unsigned char *b = (unsigned char *)heap_malloc(48);
        // Write exactly 48 bytes to a (should not clobber b's header)
        memset(a, 0xFF, 48);
        // Verify b is still usable
        memset(b, 0xEE, 48);
        int ok = 1;
        for (int i = 0; i < 48; i++)
            if (b[i] != 0xEE) {
                ok = 0;
                break;
            }
        TEST_ASSERT("header survived boundary write", ok);
        heap_free(a);
        heap_free(b);
        // And allocator still works
        void *p = heap_malloc(48);
        TEST_ASSERT("post-boundary alloc ok", p != NULL);
        heap_free(p);
    }
    TEST_PASS("header integrity");

    // free+realloc preserves subsequent block
    {
        unsigned char *a = (unsigned char *)heap_malloc(64);
        unsigned char *b = (unsigned char *)heap_malloc(64);
        memset(b, 0xBB, 64);
        heap_free(a);
        // Re-alloc same size at a's location, write it
        unsigned char *a2 = (unsigned char *)heap_malloc(64);
        memset(a2, 0xAA, 64);
        // b should be unchanged
        int ok = 1;
        for (int i = 0; i < 64; i++)
            if (b[i] != 0xBB) {
                ok = 0;
                break;
            }
        TEST_ASSERT("neighbor preserved after realloc", ok);
        heap_free(a2);
        heap_free(b);
    }
    TEST_PASS("realloc preserves neighbor");

    // edge cases

    // minimum allocation (align(1) = 16 bytes)
    {
        void *p = heap_malloc(1);
        TEST_ASSERT("min alloc ok", p != NULL);
        // Should be able to write at least 1 byte
        *(unsigned char *)p = 0x42;
        TEST_ASSERT("min alloc write", *(unsigned char *)p == 0x42);
        heap_free(p);
    }
    TEST_PASS("minimum allocation");

    // alloc sizes near align boundary (15, 16, 17)
    {
        unsigned char *p15 = (unsigned char *)heap_malloc(15);
        unsigned char *p16 = (unsigned char *)heap_malloc(16);
        unsigned char *p17 = (unsigned char *)heap_malloc(17);
        // All internally become ALIGN(n) = 16, 16, 32 bytes
        TEST_ASSERT("15-byte alloc", p15 != NULL);
        TEST_ASSERT("16-byte alloc", p16 != NULL);
        TEST_ASSERT("17-byte alloc", p17 != NULL);
        // Write exact requested sizes
        memset(p15, 0x0F, 15);
        memset(p16, 0x10, 16);
        memset(p17, 0x11, 17);
        int ok = 1;
        for (int i = 0; i < 15; i++)
            if (p15[i] != 0x0F)
                ok = 0;
        for (int i = 0; i < 16; i++)
            if (p16[i] != 0x10)
                ok = 0;
        for (int i = 0; i < 17; i++)
            if (p17[i] != 0x11)
                ok = 0;
        TEST_ASSERT("boundary sizes intact", ok);
        heap_free(p15);
        heap_free(p16);
        heap_free(p17);
    }
    TEST_PASS("ALIGN boundary sizes");

    // alloc sizes near split threshold (sizes > SLAB_MAX)
    //    Split happens when remaining >= HEADER_SIZE(32) + 16 = 48.
    {
        // Make a free block of usable space via alloc+free
        void *blk = heap_malloc(4096);
        heap_free(blk);

        // Alloc LARGE: remaining = 4096-2048 = 2048 >= 48 -> SHOULD split
        void *a = heap_malloc(LARGE);
        void *split_part = heap_malloc(LARGE); // should come from split remainder
        TEST_ASSERT("threshold: split exists", split_part != NULL);
        heap_free(a);
        heap_free(split_part);
    }
    TEST_PASS("split threshold edge");

    // exact size alloc (no waste)
    {
        // ALIGN(16) = 16, allocate exactly what a free block might have
        void *p = heap_malloc(16);
        heap_free(p);
        void *q = heap_malloc(16);
        TEST_ASSERT("exact reuse", q == p);
        heap_free(q);
    }
    TEST_PASS("exact size reuse");

    // a request matching a free block's capacity must still get every byte it
    // asked for; the redzone footer used to be carved out of the caller's region
    {
        for (unsigned long n = LARGE; n <= LARGE + 512; n += 16) {
            void *lo = heap_malloc(n);
            void *mid = heap_malloc(n);
            void *hi = heap_malloc(n);
            TEST_ASSERT("usable-size setup allocates", lo && mid && hi);

            heap_free(mid); // allocated neighbours keep the hole uncoalesced

            for (unsigned long extra = 0; extra <= 32; extra += 16) {
                void *p = heap_malloc(n + extra);
                TEST_ASSERT("exact-fit request satisfied", p != NULL);
                TEST_ASSERT("usable size covers request", heap_test_usable_size(p) >= n + extra);
                memset(p, 0xC5, n + extra); // must not reach the redzone
                heap_free(p);               // panics if it did
            }

            heap_free(lo);
            heap_free(hi);
        }
    }
    TEST_PASS("allocation is never shorter than requested");

    /*
     * heap_free used to read block->size straight out of whatever preceded the
     * pointer and locate the footer from there, so a pointer this allocator
     * never returned sent that read anywhere. Blocks now carry a tag that says
     * whether they are ours and whether they are live.
     */
    {
        void *p = heap_malloc(LARGE);
        TEST_ASSERT("alloc for tag test", p != NULL);
        TEST_ASSERT("handed-out block is tagged allocated", heap_test_is_tagged_allocated(p));

        heap_free(p);
        TEST_ASSERT("released block is no longer tagged allocated",
                    !heap_test_is_tagged_allocated(p));

        // a stack address was never ours, so it must not look allocated
        static unsigned char not_heap[64];
        memset(not_heap, 0, sizeof(not_heap));
        TEST_ASSERT("foreign memory is not tagged",
                    !heap_test_is_tagged_allocated(not_heap + sizeof(not_heap) / 2));
    }
    TEST_PASS("block headers are identifiable");

    // full lifecycle

    // alloc -> write -> free -> re-alloc -> verify clean
    {
        unsigned char *p = (unsigned char *)heap_malloc(256);
        for (int i = 0; i < 256; i++)
            p[i] = (unsigned char)(i ^ 0x55);
        heap_free(p);
        unsigned char *q = (unsigned char *)heap_malloc(256);
        // q should be at same address
        TEST_ASSERT("lifecycle: reuse addr", q == p);
        // After free, data may or may not be zeroed, but alloc should work
        // Write new pattern
        for (int i = 0; i < 256; i++)
            q[i] = (unsigned char)(i ^ 0xAA);
        int ok = 1;
        for (int i = 0; i < 256; i++)
            if (q[i] != (unsigned char)(i ^ 0xAA)) {
                ok = 0;
                break;
            }
        TEST_ASSERT("lifecycle: new pattern ok", ok);
        heap_free(q);
    }
    TEST_PASS("full lifecycle");

    // complex multi-size lifecycle (sizes > SLAB_MAX)
    {
        // Phase 1: allocate various sizes
        void *a = heap_malloc(LARGE);
        void *b = heap_malloc(4096);
        void *c = heap_malloc(LARGE);
        void *d = heap_malloc(8192);

        // Phase 2: free middle ones, creating holes
        heap_free(b);
        heap_free(c);

        // Phase 3: alloc into holes (first-fit into b's old slot)
        void *e = heap_malloc(LARGE);
        TEST_ASSERT("lifecycle2: reuse b slot", e == b);

        // Phase 4: free everything
        heap_free(a);
        heap_free(e);
        heap_free(d);

        // Phase 5: large alloc should succeed (coalesced space)
        void *f = heap_malloc(8192);
        TEST_ASSERT("lifecycle2: post-coalesce", f != NULL);
        heap_free(f);
    }
    TEST_PASS("multi-size lifecycle");

    TEST_SUITE_END("Heap Allocator");
}
