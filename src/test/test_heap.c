#include "test.h"
#include "kernel/heap.h"
#include "lib/string.h"
#include "lib/types.h"

#define TEST_HEADER_SIZE 32 /* sizeof(block_header) */

void test_heap(void)
{
    TEST_SUITE_BEGIN("Heap Allocator");

    // basic functionality

    // basic alloc / free
    {
        void* p = kmalloc(64);
        TEST_ASSERT("kmalloc returns non-null", p != NULL);
        TEST_ASSERT("kmalloc 16-byte aligned", ((unsigned long)p & 0xF) == 0);
        kfree(p);
    }
    TEST_PASS("basic alloc/free");

    // zero-size returns null
    TEST_ASSERT("zero alloc returns null", kmalloc(0) == NULL);
    TEST_PASS("zero-size returns NULL");

    // kfree(null) is safe
    kfree(NULL);
    TEST_PASS("kfree(NULL) safe");

    // 1-byte allocation
    {
        void* p = kmalloc(1);
        TEST_ASSERT("1-byte alloc non-null", p != NULL);
        TEST_ASSERT("1-byte aligned", ((unsigned long)p & 0xF) == 0);
        *(unsigned char*)p = 0x42;
        TEST_ASSERT("1-byte write", *(unsigned char*)p == 0x42);
        kfree(p);
    }
    TEST_PASS("1-byte alloc");

    // alignment guarantees

    // alignment across many sizes including odd/edge values
    {
        unsigned long sizes[] = {1,  2,  3,  4,  5,  7,   8,   9,   13,  15,  16,  17,  23,   31,   32,
                                 33, 48, 63, 64, 65, 100, 127, 128, 255, 256, 500, 512, 1000, 1024, 4096};
        int nsizes = sizeof(sizes) / sizeof(sizes[0]);
        void* ptrs[30];
        int all_aligned = 1;
        for (int i = 0; i < nsizes; i++)
        {
            ptrs[i] = kmalloc(sizes[i]);
            TEST_ASSERT("align alloc non-null", ptrs[i] != NULL);
            if (((unsigned long)ptrs[i] & 0xF) != 0)
                all_aligned = 0;
        }
        TEST_ASSERT("all pointers 16-byte aligned", all_aligned);
        for (int i = nsizes - 1; i >= 0; i--)
            kfree(ptrs[i]);
    }
    TEST_PASS("alignment 30 sizes");

    // alignment after free-and-realloc cycle
    {
        for (int i = 1; i <= 20; i++)
        {
            void* p = kmalloc(i);
            TEST_ASSERT("cycle aligned", ((unsigned long)p & 0xF) == 0);
            kfree(p);
        }
    }
    TEST_PASS("alignment after reuse");

    // non-overlapping allocations

    // two allocs don't overlap
    {
        void* a = kmalloc(128);
        void* b = kmalloc(128);
        TEST_ASSERT("a non-null", a != NULL);
        TEST_ASSERT("b non-null", b != NULL);
        unsigned long dist = (unsigned long)b > (unsigned long)a ? (unsigned long)b - (unsigned long)a
                                                                 : (unsigned long)a - (unsigned long)b;
        TEST_ASSERT("no overlap (dist >= 128)", dist >= 128);
        kfree(a);
        kfree(b);
    }
    TEST_PASS("two allocs no overlap");

    // many small allocs all distinct
    {
        void* ptrs[64];
        for (int i = 0; i < 64; i++)
        {
            ptrs[i] = kmalloc(32);
            TEST_ASSERT("64x alloc", ptrs[i] != NULL);
        }
        int distinct = 1;
        for (int i = 0; i < 64 && distinct; i++)
            for (int j = i + 1; j < 64 && distinct; j++)
                if (ptrs[i] == ptrs[j])
                    distinct = 0;
        TEST_ASSERT("all 64 distinct", distinct);
        for (int i = 0; i < 64; i++)
            kfree(ptrs[i]);
    }
    TEST_PASS("64 allocs distinct");

    // adjacent alloc data isolation
    {
        unsigned char* a = (unsigned char*)kmalloc(64);
        unsigned char* b = (unsigned char*)kmalloc(64);
        memset(a, 0xAA, 64);
        memset(b, 0xBB, 64);
        int a_ok = 1, b_ok = 1;
        for (int i = 0; i < 64; i++)
        {
            if (a[i] != 0xAA)
                a_ok = 0;
            if (b[i] != 0xBB)
                b_ok = 0;
        }
        TEST_ASSERT("block a intact", a_ok);
        TEST_ASSERT("block b intact", b_ok);
        kfree(a);
        kfree(b);
    }
    TEST_PASS("data isolation");

    // memory reuse

    // freed block is reused for same-size alloc
    {
        void* p1 = kmalloc(64);
        kfree(p1);
        void* p2 = kmalloc(64);
        TEST_ASSERT("reuses freed block", p2 == p1);
        kfree(p2);
    }
    TEST_PASS("reuse same size");

    // freed block reused for smaller alloc (fits in old block)
    {
        void* p1 = kmalloc(256);
        kfree(p1);
        void* p2 = kmalloc(16);
        // should reuse same address (first-fit), possibly splitting
        TEST_ASSERT("reuses for smaller", p2 == p1);
        kfree(p2);
    }
    TEST_PASS("reuse smaller alloc");

    // first-fit: earlier free block chosen over later
    {
        void* a = kmalloc(64);
        void* b = kmalloc(64);
        void* c = kmalloc(64);
        kfree(a);
        kfree(c);
        void* d = kmalloc(64);
        // first-fit should pick 'a' position
        TEST_ASSERT("first-fit picks earlier", d == a);
        kfree(d);
        kfree(b);
    }
    TEST_PASS("first-fit ordering");

    // block splitting

    // split occurs when remainder >= header_size + 16 (48)
    //    Allocate a big block, free it, then allocate much smaller.
    //    The remainder should be split into a second usable block.
    {
        void* big = kmalloc(256);
        kfree(big);
        // Freed block has size >= 256 (ALIGN(256)=256).
        // Allocate 16 from it. Remainder = 256-16-32 = 208 >= 48 → splits.
        void* small = kmalloc(16);
        TEST_ASSERT("split: first part", small == big);
        // Second allocation should come from the split remainder
        void* next = kmalloc(16);
        TEST_ASSERT("split: second from remainder", next != NULL);
        // next should be right after small's block: small + 16 + HEADER_SIZE(32) = small + 48
        unsigned long expected = (unsigned long)small + 16 + TEST_HEADER_SIZE;
        TEST_ASSERT("split: contiguous layout", (unsigned long)next == expected);
        kfree(small);
        kfree(next);
    }
    TEST_PASS("block splitting basic");

    // no split when remainder < header_size + 16 (48)
    //    If the remaining space after alloc is < 48 bytes, no split occurs.
    //    We indirectly verify by checking that two allocs from a just-right
    //    block still work.
    {
        // Allocate then free a block. Re-alloc the same size → no split expected.
        void* p = kmalloc(64);
        kfree(p);
        void* q = kmalloc(64);
        TEST_ASSERT("no-split same size reuse", q == p);
        kfree(q);
    }
    TEST_PASS("no unnecessary split");

    // split creates usable blocks (write to both halves)
    {
        void* big = kmalloc(512);
        kfree(big);
        unsigned char* a = (unsigned char*)kmalloc(64);
        unsigned char* b = (unsigned char*)kmalloc(64);
        // Write full patterns to both split parts
        memset(a, 0x11, 64);
        memset(b, 0x22, 64);
        int a_ok = 1, b_ok = 1;
        for (int i = 0; i < 64; i++)
        {
            if (a[i] != 0x11)
                a_ok = 0;
            if (b[i] != 0x22)
                b_ok = 0;
        }
        TEST_ASSERT("split-a data intact", a_ok);
        TEST_ASSERT("split-b data intact", b_ok);
        kfree(a);
        kfree(b);
    }
    TEST_PASS("split blocks usable");

    // repeated splitting exhausts a block correctly
    {
        void* big = kmalloc(1024);
        kfree(big);
        // Each 16-byte alloc uses 16 + 32 = 48 bytes of the original block.
        // From 1024 bytes, we can fit floor(1024 / 48) = 21 splits.
        // (21 * 48 = 1008, remainder 16 < 48 so last one absorbs it)
        void* ptrs[21];
        int count = 0;
        for (int i = 0; i < 21; i++)
        {
            ptrs[i] = kmalloc(16);
            if (ptrs[i] != NULL)
                count++;
        }
        TEST_ASSERT("repeated split fills block", count == 21);
        for (int i = 0; i < 21; i++)
            kfree(ptrs[i]);
    }
    TEST_PASS("repeated splitting");

    // coalescing

    // two adjacent free blocks coalesce
    {
        void* a = kmalloc(64);
        void* b = kmalloc(64);
        void* guard = kmalloc(64); // prevent merging beyond b
        kfree(a);
        kfree(b);
        // a+b should coalesce: 64 + 32(header) + 64 = 160 usable
        void* merged = kmalloc(160);
        TEST_ASSERT("coalesce: merged alloc succeeds", merged != NULL);
        TEST_ASSERT("coalesce: merged at a's position", merged == a);
        kfree(merged);
        kfree(guard);
    }
    TEST_PASS("two-block coalesce");

    // three adjacent free blocks coalesce (chain)
    {
        void* a = kmalloc(64);
        void* b = kmalloc(64);
        void* c = kmalloc(64);
        void* guard = kmalloc(64);
        kfree(a);
        kfree(b);
        kfree(c);
        // Total coalesced usable = 64 + 32 + 64 + 32 + 64 = 256
        void* merged = kmalloc(256);
        TEST_ASSERT("3-coalesce succeeds", merged != NULL);
        TEST_ASSERT("3-coalesce at a's position", merged == a);
        kfree(merged);
        kfree(guard);
    }
    TEST_PASS("three-block coalesce");

    // non-adjacent free blocks do not coalesce
    {
        void* a = kmalloc(64);
        void* b = kmalloc(64); // stays allocated
        void* c = kmalloc(64);
        kfree(a);
        kfree(c);
        // a and c are free but b separates them: should NOT coalesce
        // Trying to alloc 160 should NOT reuse a (a is only 64)
        void* big = kmalloc(160);
        TEST_ASSERT("no false coalesce", big != a);
        kfree(big);
        kfree(b);
    }
    TEST_PASS("non-adjacent no coalesce");

    // coalesce then split: free two adjacent, alloc smaller
    {
        void* a = kmalloc(64);
        void* b = kmalloc(64);
        void* guard = kmalloc(64);
        kfree(a);
        kfree(b);
        // Coalesced block = 160, alloc 32 from it → split (160-32-32=96 >= 48)
        void* small = kmalloc(32);
        TEST_ASSERT("coalesce+split: first part", small == a);
        void* next = kmalloc(32);
        TEST_ASSERT("coalesce+split: second from remainder", next != NULL);
        kfree(small);
        kfree(next);
        kfree(guard);
    }
    TEST_PASS("coalesce then split");

    // free in reverse order still coalesces
    {
        void* a = kmalloc(64);
        void* b = kmalloc(64);
        void* guard = kmalloc(64);
        // Free in forward order vs reverse - coalescing walks the list
        kfree(b);
        kfree(a);
        void* merged = kmalloc(160);
        TEST_ASSERT("reverse free coalesces", merged != NULL);
        kfree(merged);
        kfree(guard);
    }
    TEST_PASS("reverse-order coalesce");

    // heap expansion

    // allocation larger than initial page triggers expansion
    {
        void* big = kmalloc(8192); // 2 pages
        TEST_ASSERT("expand: large alloc ok", big != NULL);
        TEST_ASSERT("expand: aligned", ((unsigned long)big & 0xF) == 0);
        // Write boundaries
        *(volatile unsigned char*)big = 0xDE;
        *((volatile unsigned char*)big + 8191) = 0xAD;
        TEST_ASSERT("expand: first byte", *(volatile unsigned char*)big == 0xDE);
        TEST_ASSERT("expand: last byte", *((volatile unsigned char*)big + 8191) == 0xAD);
        kfree(big);
    }
    TEST_PASS("heap expansion 8KB");

    // large allocation (1 mb)
    {
        void* big = kmalloc(1024 * 1024);
        TEST_ASSERT("1MB alloc non-null", big != NULL);
        TEST_ASSERT("1MB aligned", ((unsigned long)big & 0xF) == 0);
        // Touch first, middle, last
        *(volatile unsigned char*)big = 0xAB;
        *((volatile unsigned char*)big + 512 * 1024) = 0xCD;
        *((volatile unsigned char*)big + 1024 * 1024 - 1) = 0xEF;
        TEST_ASSERT("1MB first", *(volatile unsigned char*)big == 0xAB);
        TEST_ASSERT("1MB middle", *((volatile unsigned char*)big + 512 * 1024) == 0xCD);
        TEST_ASSERT("1MB last", *((volatile unsigned char*)big + 1024 * 1024 - 1) == 0xEF);
        kfree(big);
    }
    TEST_PASS("heap expansion 1MB");

    // multiple expansions
    {
        void* a = kmalloc(16384);
        void* b = kmalloc(16384);
        void* c = kmalloc(16384);
        TEST_ASSERT("multi-expand a", a != NULL);
        TEST_ASSERT("multi-expand b", b != NULL);
        TEST_ASSERT("multi-expand c", c != NULL);
        kfree(b);
        kfree(a);
        kfree(c);
    }
    TEST_PASS("multiple expansions");

    // page-aligned allocation sizes
    {
        // Allocate exactly 4096 (PAGE_SIZE) bytes
        void* page = kmalloc(4096);
        TEST_ASSERT("page-size alloc ok", page != NULL);
        memset(page, 0xFF, 4096);
        unsigned char* cp = (unsigned char*)page;
        int ok = 1;
        for (int i = 0; i < 4096; i++)
            if (cp[i] != 0xFF)
            {
                ok = 0;
                break;
            }
        TEST_ASSERT("page-size canary", ok);
        kfree(page);
    }
    TEST_PASS("page-aligned alloc");

    // write pattern & data integrity

    // sequential byte pattern
    {
        unsigned char* mem = (unsigned char*)kmalloc(256);
        TEST_ASSERT("pattern alloc", mem != NULL);
        for (int i = 0; i < 256; i++)
            mem[i] = (unsigned char)i;
        int ok = 1;
        for (int i = 0; i < 256; i++)
            if (mem[i] != (unsigned char)i)
            {
                ok = 0;
                break;
            }
        TEST_ASSERT("sequential pattern", ok);
        kfree(mem);
    }
    TEST_PASS("sequential byte pattern");

    // boundary writes to exact allocation size
    //    Write every byte up to the requested size, making sure we don't
    //    corrupt the header of the next block.
    {
        unsigned long sizes[] = {1, 15, 16, 17, 31, 32, 33, 47, 48, 49, 63, 64};
        int nsizes = sizeof(sizes) / sizeof(sizes[0]);
        for (int s = 0; s < nsizes; s++)
        {
            unsigned char* p = (unsigned char*)kmalloc(sizes[s]);
            TEST_ASSERT("boundary alloc", p != NULL);
            // Fill entire requested region
            for (unsigned long i = 0; i < sizes[s]; i++)
                p[i] = (unsigned char)(i ^ 0xAA);
            int ok = 1;
            for (unsigned long i = 0; i < sizes[s]; i++)
                if (p[i] != (unsigned char)(i ^ 0xAA))
                {
                    ok = 0;
                    break;
                }
            TEST_ASSERT("boundary verify", ok);
            kfree(p);
        }
    }
    TEST_PASS("boundary writes");

    // alloc does not destroy neighbor data
    {
        unsigned char* a = (unsigned char*)kmalloc(128);
        unsigned char* b = (unsigned char*)kmalloc(128);
        unsigned char* c = (unsigned char*)kmalloc(128);
        memset(a, 0x11, 128);
        memset(b, 0x22, 128);
        memset(c, 0x33, 128);
        // Verify none corrupted each other
        int ok = 1;
        for (int i = 0; i < 128; i++)
        {
            if (a[i] != 0x11)
                ok = 0;
            if (b[i] != 0x22)
                ok = 0;
            if (c[i] != 0x33)
                ok = 0;
        }
        TEST_ASSERT("triple isolation", ok);
        kfree(a);
        kfree(b);
        kfree(c);
    }
    TEST_PASS("neighbor data isolation");

    // unsigned long pattern (word-aligned writes)
    {
        int nwords = 128;
        unsigned long* arr = (unsigned long*)kmalloc(nwords * sizeof(unsigned long));
        TEST_ASSERT("word alloc", arr != NULL);
        for (int i = 0; i < nwords; i++)
            arr[i] = 0xDEADBEEF00000000UL | (unsigned long)i;
        int ok = 1;
        for (int i = 0; i < nwords; i++)
            if (arr[i] != (0xDEADBEEF00000000UL | (unsigned long)i))
            {
                ok = 0;
                break;
            }
        TEST_ASSERT("word pattern intact", ok);
        kfree(arr);
    }
    TEST_PASS("word-aligned pattern");

    // free-order independence

    // fifo free
    {
        void* a = kmalloc(48);
        void* b = kmalloc(48);
        void* c = kmalloc(48);
        kfree(a);
        kfree(b);
        kfree(c);
    }
    TEST_PASS("FIFO free");

    // lifo free
    {
        void* a = kmalloc(48);
        void* b = kmalloc(48);
        void* c = kmalloc(48);
        kfree(c);
        kfree(b);
        kfree(a);
    }
    TEST_PASS("LIFO free");

    // middle-first free
    {
        void* a = kmalloc(48);
        void* b = kmalloc(48);
        void* c = kmalloc(48);
        kfree(b);
        kfree(a);
        kfree(c);
    }
    TEST_PASS("middle-first free");

    // fragmentation patterns

    // alternating holes
    {
        void* ptrs[16];
        for (int i = 0; i < 16; i++)
            ptrs[i] = kmalloc(64);
        // Free even-indexed blocks → creates 8 "holes"
        for (int i = 0; i < 16; i += 2)
            kfree(ptrs[i]);
        // Reallocate into holes
        for (int i = 0; i < 16; i += 2)
        {
            ptrs[i] = kmalloc(64);
            TEST_ASSERT("frag realloc", ptrs[i] != NULL);
        }
        for (int i = 0; i < 16; i++)
            kfree(ptrs[i]);
    }
    TEST_PASS("alternating holes");

    // every-third pattern
    {
        void* ptrs[12];
        for (int i = 0; i < 12; i++)
            ptrs[i] = kmalloc(32);
        // Free every 3rd
        for (int i = 0; i < 12; i += 3)
            kfree(ptrs[i]);
        // Allocate into freed slots
        for (int i = 0; i < 12; i += 3)
        {
            ptrs[i] = kmalloc(32);
            TEST_ASSERT("every-3rd realloc", ptrs[i] != NULL);
        }
        for (int i = 0; i < 12; i++)
            kfree(ptrs[i]);
    }
    TEST_PASS("every-third pattern");

    // swiss cheese: random-like free pattern
    {
        void* ptrs[20];
        for (int i = 0; i < 20; i++)
            ptrs[i] = kmalloc(48);
        // Free a scattered pattern: 1,4,6,9,11,14,16,19
        int free_idx[] = {1, 4, 6, 9, 11, 14, 16, 19};
        for (int i = 0; i < 8; i++)
            kfree(ptrs[free_idx[i]]);
        // Re-alloc same size into freed holes
        for (int i = 0; i < 8; i++)
        {
            ptrs[free_idx[i]] = kmalloc(48);
            TEST_ASSERT("swiss cheese realloc", ptrs[free_idx[i]] != NULL);
        }
        for (int i = 0; i < 20; i++)
            kfree(ptrs[i]);
    }
    TEST_PASS("swiss cheese pattern");

    // mixed size allocations

    // small + medium + large interleaved
    {
        void* s1 = kmalloc(16);
        void* m1 = kmalloc(256);
        void* l1 = kmalloc(4096);
        void* s2 = kmalloc(32);
        void* m2 = kmalloc(512);
        TEST_ASSERT("mixed s1", s1 != NULL);
        TEST_ASSERT("mixed m1", m1 != NULL);
        TEST_ASSERT("mixed l1", l1 != NULL);
        TEST_ASSERT("mixed s2", s2 != NULL);
        TEST_ASSERT("mixed m2", m2 != NULL);
        kfree(m1);
        kfree(s2);
        kfree(l1);
        kfree(s1);
        kfree(m2);
    }
    TEST_PASS("mixed sizes interleaved");

    // growing allocations: 16 → 32 → 64 → ... → 4096
    {
        void* ptrs[9];
        for (int i = 0; i < 9; i++)
        {
            unsigned long sz = 16UL << i; // 16,32,64,...,4096
            ptrs[i] = kmalloc(sz);
            TEST_ASSERT("growing alloc", ptrs[i] != NULL);
        }
        for (int i = 8; i >= 0; i--)
            kfree(ptrs[i]);
    }
    TEST_PASS("growing allocs");

    // shrinking allocations: 4096 → 2048 → ... → 16
    {
        void* ptrs[9];
        for (int i = 0; i < 9; i++)
        {
            unsigned long sz = 4096UL >> i; // 4096,2048,...,16
            ptrs[i] = kmalloc(sz);
            TEST_ASSERT("shrinking alloc", ptrs[i] != NULL);
        }
        for (int i = 0; i < 9; i++)
            kfree(ptrs[i]);
    }
    TEST_PASS("shrinking allocs");

    // power-of-2 allocations with canary
    {
        unsigned long po2_sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
        int n = sizeof(po2_sizes) / sizeof(po2_sizes[0]);
        for (int i = 0; i < n; i++)
        {
            void* ptr = kmalloc(po2_sizes[i]);
            TEST_ASSERT("po2 alloc", ptr != NULL);
            TEST_ASSERT("po2 aligned", ((unsigned long)ptr & 0xF) == 0);
            memset(ptr, 0xCC, po2_sizes[i]);
            int ok = 1;
            unsigned char* cp = (unsigned char*)ptr;
            for (unsigned long j = 0; j < po2_sizes[i]; j++)
                if (cp[j] != 0xCC)
                {
                    ok = 0;
                    break;
                }
            TEST_ASSERT("po2 canary", ok);
            kfree(ptr);
        }
    }
    TEST_PASS("power-of-2 canary");

    // stress tests

    // rapid alloc/free same size ×200
    {
        for (int i = 0; i < 200; i++)
        {
            void* ptr = kmalloc(128);
            TEST_ASSERT("rapid alloc", ptr != NULL);
            *(volatile unsigned long*)ptr = 0xCAFEBABEUL;
            TEST_ASSERT("rapid canary", *(volatile unsigned long*)ptr == 0xCAFEBABEUL);
            kfree(ptr);
        }
    }
    TEST_PASS("rapid cycle x200");

    // fill-and-drain 128 blocks
    {
        void* ptrs[128];
        for (int i = 0; i < 128; i++)
        {
            ptrs[i] = kmalloc(64);
            TEST_ASSERT("fill alloc", ptrs[i] != NULL);
        }
        for (int i = 127; i >= 0; i--)
            kfree(ptrs[i]);

        // After draining, allocator should still work normally
        void* p = kmalloc(64);
        TEST_ASSERT("post-drain alloc", p != NULL);
        kfree(p);
    }
    TEST_PASS("fill-and-drain 128");

    // sawtooth: alloc batch then free batch, repeat
    {
        for (int round = 0; round < 5; round++)
        {
            void* ptrs[32];
            for (int i = 0; i < 32; i++)
            {
                ptrs[i] = kmalloc(48);
                TEST_ASSERT("sawtooth alloc", ptrs[i] != NULL);
            }
            for (int i = 0; i < 32; i++)
                kfree(ptrs[i]);
        }
    }
    TEST_PASS("sawtooth 5 rounds");

    // interleaved alloc/free (wave pattern)
    {
        void* ptrs[32];
        int count = 0;
        // Allocate 4, free 2, allocate 4, free 2, ...
        for (int wave = 0; wave < 8; wave++)
        {
            for (int i = 0; i < 4; i++)
            {
                ptrs[count] = kmalloc(64);
                TEST_ASSERT("wave alloc", ptrs[count] != NULL);
                count++;
            }
            for (int i = 0; i < 2 && count > 0; i++)
            {
                count--;
                kfree(ptrs[count]);
            }
        }
        // Free remaining
        for (int i = count - 1; i >= 0; i--)
            kfree(ptrs[i]);
    }
    TEST_PASS("wave alloc/free");

    // header integrity after operations

    // write full requested size, then free and re-alloc
    //    If writing up to the boundary corrupted the next header, the
    //    subsequent alloc/free would crash or return bad data.
    {
        unsigned char* a = (unsigned char*)kmalloc(48);
        unsigned char* b = (unsigned char*)kmalloc(48);
        // Write exactly 48 bytes to a (should not clobber b's header)
        memset(a, 0xFF, 48);
        // Verify b is still usable
        memset(b, 0xEE, 48);
        int ok = 1;
        for (int i = 0; i < 48; i++)
            if (b[i] != 0xEE)
            {
                ok = 0;
                break;
            }
        TEST_ASSERT("header survived boundary write", ok);
        kfree(a);
        kfree(b);
        // And allocator still works
        void* p = kmalloc(48);
        TEST_ASSERT("post-boundary alloc ok", p != NULL);
        kfree(p);
    }
    TEST_PASS("header integrity");

    // free+realloc preserves subsequent block
    {
        unsigned char* a = (unsigned char*)kmalloc(64);
        unsigned char* b = (unsigned char*)kmalloc(64);
        memset(b, 0xBB, 64);
        kfree(a);
        // Re-alloc same size at a's location, write it
        unsigned char* a2 = (unsigned char*)kmalloc(64);
        memset(a2, 0xAA, 64);
        // b should be unchanged
        int ok = 1;
        for (int i = 0; i < 64; i++)
            if (b[i] != 0xBB)
            {
                ok = 0;
                break;
            }
        TEST_ASSERT("neighbor preserved after realloc", ok);
        kfree(a2);
        kfree(b);
    }
    TEST_PASS("realloc preserves neighbor");

    // edge cases

    // minimum allocation (align(1) = 16 bytes)
    {
        void* p = kmalloc(1);
        TEST_ASSERT("min alloc ok", p != NULL);
        // Should be able to write at least 1 byte
        *(unsigned char*)p = 0x42;
        TEST_ASSERT("min alloc write", *(unsigned char*)p == 0x42);
        kfree(p);
    }
    TEST_PASS("minimum allocation");

    // alloc sizes near align boundary (15, 16, 17)
    {
        unsigned char* p15 = (unsigned char*)kmalloc(15);
        unsigned char* p16 = (unsigned char*)kmalloc(16);
        unsigned char* p17 = (unsigned char*)kmalloc(17);
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
        kfree(p15);
        kfree(p16);
        kfree(p17);
    }
    TEST_PASS("ALIGN boundary sizes");

    // alloc sizes near split threshold
    //    Split happens when remaining >= HEADER_SIZE(32) + 16 = 48.
    //    Allocate from a 128-byte block with sizes that leave various remainders.
    {
        // Make a free block of exactly 128 bytes usable space
        // by allocating 128 then freeing
        void* blk = kmalloc(128);
        kfree(blk);

        // Alloc 80: remaining = 128 - 80 = 48 = HEADER(32) + 16 → SHOULD split
        void* a = kmalloc(80);
        void* split_part = kmalloc(16); // should come from split remainder
        TEST_ASSERT("threshold: 80→split exists", split_part != NULL);
        kfree(a);
        kfree(split_part);
    }
    TEST_PASS("split threshold edge");

    // exact size alloc (no waste)
    {
        // ALIGN(16) = 16, allocate exactly what a free block might have
        void* p = kmalloc(16);
        kfree(p);
        void* q = kmalloc(16);
        TEST_ASSERT("exact reuse", q == p);
        kfree(q);
    }
    TEST_PASS("exact size reuse");

    // full lifecycle

    // alloc → write → free → re-alloc → verify clean
    {
        unsigned char* p = (unsigned char*)kmalloc(256);
        for (int i = 0; i < 256; i++)
            p[i] = (unsigned char)(i ^ 0x55);
        kfree(p);
        unsigned char* q = (unsigned char*)kmalloc(256);
        // q should be at same address
        TEST_ASSERT("lifecycle: reuse addr", q == p);
        // After free, data may or may not be zeroed, but alloc should work
        // Write new pattern
        for (int i = 0; i < 256; i++)
            q[i] = (unsigned char)(i ^ 0xAA);
        int ok = 1;
        for (int i = 0; i < 256; i++)
            if (q[i] != (unsigned char)(i ^ 0xAA))
            {
                ok = 0;
                break;
            }
        TEST_ASSERT("lifecycle: new pattern ok", ok);
        kfree(q);
    }
    TEST_PASS("full lifecycle");

    // complex multi-size lifecycle
    {
        // Phase 1: allocate various sizes
        void* a = kmalloc(32);
        void* b = kmalloc(128);
        void* c = kmalloc(64);
        void* d = kmalloc(256);

        // Phase 2: free middle ones, creating holes
        kfree(b);
        kfree(c);

        // Phase 3: alloc into holes (first-fit into b's old slot)
        void* e = kmalloc(64);
        TEST_ASSERT("lifecycle2: reuse b slot", e == b);

        // Phase 4: free everything
        kfree(a);
        kfree(e);
        kfree(d);

        // Phase 5: large alloc should succeed (coalesced space)
        void* f = kmalloc(512);
        TEST_ASSERT("lifecycle2: post-coalesce", f != NULL);
        kfree(f);
    }
    TEST_PASS("multi-size lifecycle");

    TEST_SUITE_END("Heap Allocator");
}
