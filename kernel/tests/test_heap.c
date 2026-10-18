/*
 * test_heap.c - The heap: alignment, first-fit reuse, splitting, coalescing and redzones.
 */

#include "test.h"

#include <stdbool.h>
#include <stddef.h>

#include "string.h"

#include "mm/heap.h"

#define TEST_HEADER_SIZE 32   // sizeof(struct heap_block_header)
#define TEST_FOOTER_SIZE 16   // sizeof(struct heap_block_footer)
#define LARGE            2048 // Above HEAP_SLAB_MAX (1024), so first fit serves it

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

    TEST_ASSERT("zero alloc returns null", heap_malloc(0) == NULL);

    TEST_ASSERT("huge alloc returns null", heap_malloc(0xFFFFFFFFFFFFFFFFULL) == NULL);

    // heap_free(null) is safe
    heap_free(NULL);

    // 1-byte allocation
    {
        void *p = heap_malloc(1);
        TEST_ASSERT("1-byte alloc non-null", p != NULL);
        TEST_ASSERT("1-byte aligned", ((unsigned long)p & 0xF) == 0);
        *(unsigned char *)p = 0x42;
        TEST_ASSERT("1-byte write", *(unsigned char *)p == 0x42);
        heap_free(p);
    }

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
            if (((unsigned long)ptrs[i] & 0xF) != 0) {
                all_aligned = 0;
            }
        }
        TEST_ASSERT("all pointers 16-byte aligned", all_aligned);
        for (int i = nsizes - 1; i >= 0; i--) {
            heap_free(ptrs[i]);
        }
    }

    // alignment after free-and-realloc cycle
    {
        for (int i = 1; i <= 20; i++) {
            void *p = heap_malloc(i);
            TEST_ASSERT("cycle aligned", ((unsigned long)p & 0xF) == 0);
            heap_free(p);
        }
    }

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

    // many small allocs all distinct
    {
        void *ptrs[64];
        for (int i = 0; i < 64; i++) {
            ptrs[i] = heap_malloc(32);
            TEST_ASSERT("64x alloc", ptrs[i] != NULL);
        }
        int distinct = 1;
        for (int i = 0; i < 64 && distinct; i++) {
            for (int j = i + 1; j < 64 && distinct; j++) {
                if (ptrs[i] == ptrs[j]) {
                    distinct = 0;
                }
            }
        }
        TEST_ASSERT("all 64 distinct", distinct);
        for (int i = 0; i < 64; i++) {
            heap_free(ptrs[i]);
        }
    }

    // adjacent alloc data isolation
    {
        unsigned char *a = (unsigned char *)heap_malloc(64);
        unsigned char *b = (unsigned char *)heap_malloc(64);
        memset(a, 0xAA, 64);
        memset(b, 0xBB, 64);
        int a_ok = 1, b_ok = 1;
        for (int i = 0; i < 64; i++) {
            if (a[i] != 0xAA) {
                a_ok = 0;
            }
            if (b[i] != 0xBB) {
                b_ok = 0;
            }
        }
        TEST_ASSERT("block a intact", a_ok);
        TEST_ASSERT("block b intact", b_ok);
        heap_free(a);
        heap_free(b);
    }

    // memory reuse

    // freed block is reused for same-size alloc
    {
        void *p1 = heap_malloc(64);
        heap_free(p1);
        void *p2 = heap_malloc(64);
        TEST_ASSERT("reuses freed block", p2 == p1);
        heap_free(p2);
    }

    // freed block reused for smaller alloc
    {
        void *p1 = heap_malloc(4096);
        heap_free(p1);
        void *p2 = heap_malloc(LARGE);
        TEST_ASSERT("reuses for smaller", p2 == p1);
        heap_free(p2);
    }

    // first fit: the earlier free block wins over a later one
    {
        void *a = heap_malloc(LARGE);
        void *b = heap_malloc(LARGE);
        void *c = heap_malloc(LARGE);
        heap_free(a);
        heap_free(c);
        void *d = heap_malloc(LARGE);
        TEST_ASSERT("first-fit picks earlier", d == a);
        heap_free(d);
        heap_free(b);
    }

    // Placement asserts from here need every earlier hole filled: fill until the heap grows.
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

    // block splitting

    // a split needs 48 bytes past the request and its footer: a header and 16 usable
    {
        void *big = heap_malloc(8192);
        heap_free(big);
        void *small = heap_malloc(LARGE);
        TEST_ASSERT("split: first part", small == big);
        void *next = heap_malloc(LARGE);
        TEST_ASSERT("split: second from remainder", next != NULL);
        unsigned long expected = (unsigned long)small + LARGE + TEST_HEADER_SIZE + TEST_FOOTER_SIZE;
        TEST_ASSERT("split: contiguous layout", (unsigned long)next == expected);
        heap_free(small);
        heap_free(next);
    }

    // no split when the remainder is under 48 bytes
    {
        void *p = heap_malloc(LARGE);
        heap_free(p);
        void *q = heap_malloc(LARGE);
        TEST_ASSERT("no-split same size reuse", q == p);
        heap_free(q);
    }

    // both halves of a split are usable to their last byte
    {
        void *big = heap_malloc(8192);
        heap_free(big);
        unsigned char *a = (unsigned char *)heap_malloc(LARGE);
        unsigned char *b = (unsigned char *)heap_malloc(LARGE);
        memset(a, 0x11, LARGE);
        memset(b, 0x22, LARGE);
        int a_ok = 1, b_ok = 1;
        for (int i = 0; i < LARGE; i++) {
            if (a[i] != 0x11) {
                a_ok = 0;
            }
            if (b[i] != 0x22) {
                b_ok = 0;
            }
        }
        TEST_ASSERT("split-a data intact", a_ok);
        TEST_ASSERT("split-b data intact", b_ok);
        heap_free(a);
        heap_free(b);
    }

    // repeated splitting carves several blocks out of one
    {
        void *big = heap_malloc(8192);
        heap_free(big);
        // Each LARGE takes LARGE + 48 with its header and footer, so 8192 holds three.
        void *ptrs[3];
        int count = 0;
        for (int i = 0; i < 3; i++) {
            ptrs[i] = heap_malloc(LARGE);
            if (ptrs[i] != NULL) {
                count++;
            }
        }
        TEST_ASSERT("repeated split fills block", count == 3);
        for (int i = 0; i < 3; i++) {
            heap_free(ptrs[i]);
        }
    }

    // coalescing

    // two adjacent free blocks coalesce
    {
        void *a = heap_malloc(LARGE);
        void *b = heap_malloc(LARGE);
        void *guard = heap_malloc(LARGE); // keeps the merge from reaching past b
        heap_free(a);
        heap_free(b);
        // The merged block is 2 * (LARGE + 16) + 32 bytes: room for 4128 and its footer.
        void *merged = heap_malloc(4128);
        TEST_ASSERT("coalesce: merged alloc succeeds", merged != NULL);
        TEST_ASSERT("coalesce: merged at a's position", merged == a);
        heap_free(merged);
        heap_free(guard);
    }

    // three adjacent free blocks coalesce (chain)
    {
        void *a = heap_malloc(LARGE);
        void *b = heap_malloc(LARGE);
        void *c = heap_malloc(LARGE);
        void *guard = heap_malloc(LARGE);
        heap_free(a);
        heap_free(b);
        heap_free(c);
        // Three merge into 3 * (LARGE + 16) + 2 * 32 bytes: room for 6208 and its footer.
        void *merged = heap_malloc(6208);
        TEST_ASSERT("3-coalesce succeeds", merged != NULL);
        TEST_ASSERT("3-coalesce at a's position", merged == a);
        heap_free(merged);
        heap_free(guard);
    }

    // non-adjacent free blocks do not coalesce
    {
        void *a = heap_malloc(LARGE);
        void *b = heap_malloc(LARGE);
        void *c = heap_malloc(LARGE);
        heap_free(a);
        heap_free(c);
        // b keeps a and c apart, so a cannot hold 4128.
        void *big = heap_malloc(4128);
        TEST_ASSERT("no false coalesce", big != a);
        heap_free(big);
        heap_free(b);
    }

    // coalesce then split: free two adjacent, alloc smaller
    {
        void *a = heap_malloc(LARGE);
        void *b = heap_malloc(LARGE);
        void *guard = heap_malloc(LARGE);
        heap_free(a);
        heap_free(b);
        void *small = heap_malloc(LARGE);
        TEST_ASSERT("coalesce+split: first part", small == a);
        void *next = heap_malloc(LARGE);
        TEST_ASSERT("coalesce+split: second from remainder", next != NULL);
        heap_free(small);
        heap_free(next);
        heap_free(guard);
    }

    // free in reverse order still coalesces
    {
        void *a = heap_malloc(LARGE);
        void *b = heap_malloc(LARGE);
        void *guard = heap_malloc(LARGE);
        heap_free(b);
        heap_free(a);
        void *merged = heap_malloc(4128);
        TEST_ASSERT("reverse free coalesces", merged != NULL);
        heap_free(merged);
        heap_free(guard);
    }

    for (int i = layout_nfill - 1; i >= 0; i--) {
        heap_free(layout_fill[i]);
    }

    // heap expansion

    // allocation larger than initial page triggers expansion
    {
        void *big = heap_malloc(8192);
        TEST_ASSERT("expand: large alloc ok", big != NULL);
        TEST_ASSERT("expand: aligned", ((unsigned long)big & 0xF) == 0);
        *(volatile unsigned char *)big = 0xDE;
        *((volatile unsigned char *)big + 8191) = 0xAD;
        TEST_ASSERT("expand: first byte", *(volatile unsigned char *)big == 0xDE);
        TEST_ASSERT("expand: last byte", *((volatile unsigned char *)big + 8191) == 0xAD);
        heap_free(big);
    }

    // large allocation (1 mb)
    {
        void *big = heap_malloc(1024 * 1024);
        TEST_ASSERT("1MB alloc non-null", big != NULL);
        TEST_ASSERT("1MB aligned", ((unsigned long)big & 0xF) == 0);
        *(volatile unsigned char *)big = 0xAB;
        *((volatile unsigned char *)big + 512 * 1024) = 0xCD;
        *((volatile unsigned char *)big + 1024 * 1024 - 1) = 0xEF;
        TEST_ASSERT("1MB first", *(volatile unsigned char *)big == 0xAB);
        TEST_ASSERT("1MB middle", *((volatile unsigned char *)big + 512 * 1024) == 0xCD);
        TEST_ASSERT("1MB last", *((volatile unsigned char *)big + 1024 * 1024 - 1) == 0xEF);
        heap_free(big);
    }

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

    // page-aligned allocation sizes
    {
        void *page = heap_malloc(4096);
        TEST_ASSERT("page-size alloc ok", page != NULL);
        memset(page, 0xFF, 4096);
        unsigned char *cp = (unsigned char *)page;
        int ok = 1;
        for (int i = 0; i < 4096; i++) {
            if (cp[i] != 0xFF) {
                ok = 0;
                break;
            }
        }
        TEST_ASSERT("page-size canary", ok);
        heap_free(page);
    }

    // write pattern & data integrity

    // sequential byte pattern
    {
        unsigned char *mem = (unsigned char *)heap_malloc(256);
        TEST_ASSERT("pattern alloc", mem != NULL);
        for (int i = 0; i < 256; i++) {
            mem[i] = (unsigned char)i;
        }
        int ok = 1;
        for (int i = 0; i < 256; i++) {
            if (mem[i] != (unsigned char)i) {
                ok = 0;
                break;
            }
        }
        TEST_ASSERT("sequential pattern", ok);
        heap_free(mem);
    }

    // every byte up to the requested size is writable
    {
        unsigned long sizes[] = {1, 15, 16, 17, 31, 32, 33, 47, 48, 49, 63, 64};
        int nsizes = sizeof(sizes) / sizeof(sizes[0]);
        for (int s = 0; s < nsizes; s++) {
            unsigned char *p = (unsigned char *)heap_malloc(sizes[s]);
            TEST_ASSERT("boundary alloc", p != NULL);
            for (unsigned long i = 0; i < sizes[s]; i++) {
                p[i] = (unsigned char)(i ^ 0xAA);
            }
            int ok = 1;
            for (unsigned long i = 0; i < sizes[s]; i++) {
                if (p[i] != (unsigned char)(i ^ 0xAA)) {
                    ok = 0;
                    break;
                }
            }
            TEST_ASSERT("boundary verify", ok);
            heap_free(p);
        }
    }

    // alloc does not destroy neighbor data
    {
        unsigned char *a = (unsigned char *)heap_malloc(128);
        unsigned char *b = (unsigned char *)heap_malloc(128);
        unsigned char *c = (unsigned char *)heap_malloc(128);
        memset(a, 0x11, 128);
        memset(b, 0x22, 128);
        memset(c, 0x33, 128);
        int ok = 1;
        for (int i = 0; i < 128; i++) {
            if (a[i] != 0x11) {
                ok = 0;
            }
            if (b[i] != 0x22) {
                ok = 0;
            }
            if (c[i] != 0x33) {
                ok = 0;
            }
        }
        TEST_ASSERT("triple isolation", ok);
        heap_free(a);
        heap_free(b);
        heap_free(c);
    }

    // unsigned long pattern (word-aligned writes)
    {
        int nwords = 128;
        unsigned long *arr = (unsigned long *)heap_malloc(nwords * sizeof(unsigned long));
        TEST_ASSERT("word alloc", arr != NULL);
        for (int i = 0; i < nwords; i++) {
            arr[i] = 0xDEADBEEF00000000UL | (unsigned long)i;
        }
        int ok = 1;
        for (int i = 0; i < nwords; i++) {
            if (arr[i] != (0xDEADBEEF00000000UL | (unsigned long)i)) {
                ok = 0;
                break;
            }
        }
        TEST_ASSERT("word pattern intact", ok);
        heap_free(arr);
    }

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

    // lifo free
    {
        void *a = heap_malloc(48);
        void *b = heap_malloc(48);
        void *c = heap_malloc(48);
        heap_free(c);
        heap_free(b);
        heap_free(a);
    }

    // middle-first free
    {
        void *a = heap_malloc(48);
        void *b = heap_malloc(48);
        void *c = heap_malloc(48);
        heap_free(b);
        heap_free(a);
        heap_free(c);
    }

    // fragmentation patterns

    // alternating holes
    {
        void *ptrs[16];
        for (int i = 0; i < 16; i++) {
            ptrs[i] = heap_malloc(64);
        }
        // Free even-indexed blocks -> creates 8 "holes"
        for (int i = 0; i < 16; i += 2) {
            heap_free(ptrs[i]);
        }
        // Reallocate into holes
        for (int i = 0; i < 16; i += 2) {
            ptrs[i] = heap_malloc(64);
            TEST_ASSERT("frag realloc", ptrs[i] != NULL);
        }
        for (int i = 0; i < 16; i++) {
            heap_free(ptrs[i]);
        }
    }

    // every-third pattern
    {
        void *ptrs[12];
        for (int i = 0; i < 12; i++) {
            ptrs[i] = heap_malloc(32);
        }
        // Free every 3rd
        for (int i = 0; i < 12; i += 3) {
            heap_free(ptrs[i]);
        }
        // Allocate into freed slots
        for (int i = 0; i < 12; i += 3) {
            ptrs[i] = heap_malloc(32);
            TEST_ASSERT("every-3rd realloc", ptrs[i] != NULL);
        }
        for (int i = 0; i < 12; i++) {
            heap_free(ptrs[i]);
        }
    }

    // swiss cheese: random-like free pattern
    {
        void *ptrs[20];
        for (int i = 0; i < 20; i++) {
            ptrs[i] = heap_malloc(48);
        }
        // Free a scattered pattern: 1,4,6,9,11,14,16,19
        int free_idx[] = {1, 4, 6, 9, 11, 14, 16, 19};
        for (int i = 0; i < 8; i++) {
            heap_free(ptrs[free_idx[i]]);
        }
        // Re-alloc same size into freed holes
        for (int i = 0; i < 8; i++) {
            ptrs[free_idx[i]] = heap_malloc(48);
            TEST_ASSERT("swiss cheese realloc", ptrs[free_idx[i]] != NULL);
        }
        for (int i = 0; i < 20; i++) {
            heap_free(ptrs[i]);
        }
    }

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

    // growing allocations: 16 -> 32 -> 64 -> ... -> 4096
    {
        void *ptrs[9];
        for (int i = 0; i < 9; i++) {
            unsigned long sz = 16UL << i; // 16,32,64,...,4096
            ptrs[i] = heap_malloc(sz);
            TEST_ASSERT("growing alloc", ptrs[i] != NULL);
        }
        for (int i = 8; i >= 0; i--) {
            heap_free(ptrs[i]);
        }
    }

    // shrinking allocations: 4096 -> 2048 -> ... -> 16
    {
        void *ptrs[9];
        for (int i = 0; i < 9; i++) {
            unsigned long sz = 4096UL >> i; // 4096,2048,...,16
            ptrs[i] = heap_malloc(sz);
            TEST_ASSERT("shrinking alloc", ptrs[i] != NULL);
        }
        for (int i = 0; i < 9; i++) {
            heap_free(ptrs[i]);
        }
    }

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
            for (unsigned long j = 0; j < po2_sizes[i]; j++) {
                if (cp[j] != 0xCC) {
                    ok = 0;
                    break;
                }
            }
            TEST_ASSERT("po2 canary", ok);
            heap_free(ptr);
        }
    }

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

    // fill-and-drain 128 blocks
    {
        void *ptrs[128];
        for (int i = 0; i < 128; i++) {
            ptrs[i] = heap_malloc(64);
            TEST_ASSERT("fill alloc", ptrs[i] != NULL);
        }
        for (int i = 127; i >= 0; i--) {
            heap_free(ptrs[i]);
        }

        // After draining, allocator should still work normally
        void *p = heap_malloc(64);
        TEST_ASSERT("post-drain alloc", p != NULL);
        heap_free(p);
    }

    // sawtooth: alloc batch then free batch, repeat
    {
        for (int round = 0; round < 5; round++) {
            void *ptrs[32];
            for (int i = 0; i < 32; i++) {
                ptrs[i] = heap_malloc(48);
                TEST_ASSERT("sawtooth alloc", ptrs[i] != NULL);
            }
            for (int i = 0; i < 32; i++) {
                heap_free(ptrs[i]);
            }
        }
    }

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
        for (int i = count - 1; i >= 0; i--) {
            heap_free(ptrs[i]);
        }
    }

    // header integrity after operations

    // filling a block to its last byte leaves the next block's header intact
    {
        unsigned char *a = (unsigned char *)heap_malloc(48);
        unsigned char *b = (unsigned char *)heap_malloc(48);
        memset(a, 0xFF, 48);
        memset(b, 0xEE, 48);
        int ok = 1;
        for (int i = 0; i < 48; i++) {
            if (b[i] != 0xEE) {
                ok = 0;
                break;
            }
        }
        TEST_ASSERT("header survived boundary write", ok);
        heap_free(a);
        heap_free(b);
        void *p = heap_malloc(48);
        TEST_ASSERT("post-boundary alloc ok", p != NULL);
        heap_free(p);
    }

    // free+realloc preserves subsequent block
    {
        unsigned char *a = (unsigned char *)heap_malloc(64);
        unsigned char *b = (unsigned char *)heap_malloc(64);
        memset(b, 0xBB, 64);
        heap_free(a);
        unsigned char *a2 = (unsigned char *)heap_malloc(64);
        memset(a2, 0xAA, 64);
        int ok = 1;
        for (int i = 0; i < 64; i++) {
            if (b[i] != 0xBB) {
                ok = 0;
                break;
            }
        }
        TEST_ASSERT("neighbor preserved after realloc", ok);
        heap_free(a2);
        heap_free(b);
    }

    // edge cases

    // minimum allocation
    {
        void *p = heap_malloc(1);
        TEST_ASSERT("min alloc ok", p != NULL);
        *(unsigned char *)p = 0x42;
        TEST_ASSERT("min alloc write", *(unsigned char *)p == 0x42);
        heap_free(p);
    }

    // alloc sizes near align boundary (15, 16, 17)
    {
        unsigned char *p15 = (unsigned char *)heap_malloc(15);
        unsigned char *p16 = (unsigned char *)heap_malloc(16);
        unsigned char *p17 = (unsigned char *)heap_malloc(17);
        TEST_ASSERT("15-byte alloc", p15 != NULL);
        TEST_ASSERT("16-byte alloc", p16 != NULL);
        TEST_ASSERT("17-byte alloc", p17 != NULL);
        memset(p15, 0x0F, 15);
        memset(p16, 0x10, 16);
        memset(p17, 0x11, 17);
        int ok = 1;
        for (int i = 0; i < 15; i++) {
            if (p15[i] != 0x0F) {
                ok = 0;
            }
        }
        for (int i = 0; i < 16; i++) {
            if (p16[i] != 0x10) {
                ok = 0;
            }
        }
        for (int i = 0; i < 17; i++) {
            if (p17[i] != 0x11) {
                ok = 0;
            }
        }
        TEST_ASSERT("boundary sizes intact", ok);
        heap_free(p15);
        heap_free(p16);
        heap_free(p17);
    }

    // LARGE from a freed 4096 block leaves a remainder big enough for another LARGE
    {
        void *blk = heap_malloc(4096);
        heap_free(blk);

        void *a = heap_malloc(LARGE);
        void *split_part = heap_malloc(LARGE);
        TEST_ASSERT("threshold: split exists", split_part != NULL);
        heap_free(a);
        heap_free(split_part);
    }

    // exact size alloc
    {
        void *p = heap_malloc(16);
        heap_free(p);
        void *q = heap_malloc(16);
        TEST_ASSERT("exact reuse", q == p);
        heap_free(q);
    }

    // a request that fills a free block still gets every byte: the footer comes out of the block
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

    // A block's tag says whether it is ours and live, so heap_free refuses pointers it never gave.
    {
        void *p = heap_malloc(LARGE);
        TEST_ASSERT("alloc for tag test", p != NULL);
        TEST_ASSERT("handed-out block is tagged allocated", heap_test_is_tagged_allocated(p));

        heap_free(p);
        TEST_ASSERT("released block is no longer tagged allocated",
                    !heap_test_is_tagged_allocated(p));

        // a static buffer was never ours, so it must not look allocated
        static unsigned char not_heap[64];
        memset(not_heap, 0, sizeof(not_heap));
        TEST_ASSERT("foreign memory is not tagged",
                    !heap_test_is_tagged_allocated(not_heap + sizeof(not_heap) / 2));
    }

    // full lifecycle

    // alloc, write, free, then alloc again at the same address
    {
        unsigned char *p = (unsigned char *)heap_malloc(256);
        for (int i = 0; i < 256; i++) {
            p[i] = (unsigned char)(i ^ 0x55);
        }
        heap_free(p);
        unsigned char *q = (unsigned char *)heap_malloc(256);
        TEST_ASSERT("lifecycle: reuse addr", q == p);
        for (int i = 0; i < 256; i++) {
            q[i] = (unsigned char)(i ^ 0xAA);
        }
        int ok = 1;
        for (int i = 0; i < 256; i++) {
            if (q[i] != (unsigned char)(i ^ 0xAA)) {
                ok = 0;
                break;
            }
        }
        TEST_ASSERT("lifecycle: new pattern ok", ok);
        heap_free(q);
    }

    // multi-size lifecycle
    {
        void *a = heap_malloc(LARGE);
        void *b = heap_malloc(4096);
        void *c = heap_malloc(LARGE);
        void *d = heap_malloc(8192);

        heap_free(b);
        heap_free(c);

        // First fit takes the lower freed slot, which depends on the holes boot left.
        void *e = heap_malloc(LARGE);
        void *lowest = ((unsigned long)b < (unsigned long)c) ? b : c;
        TEST_ASSERT("lifecycle2: reuse the lowest freed slot", e == lowest);

        heap_free(a);
        heap_free(e);
        heap_free(d);

        void *f = heap_malloc(8192);
        TEST_ASSERT("lifecycle2: post-coalesce", f != NULL);
        heap_free(f);
    }

    TEST_SUITE_END("Heap Allocator");
}
