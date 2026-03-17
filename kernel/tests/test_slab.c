#include "test.h"
#include "mm/slab.h"
#include "mm/heap.h"
#include "string.h"
#include "types.h"

void test_slab(void)
{
    TEST_SUITE_BEGIN("Slab Allocator");

    // basic alloc / free for each size class
    {
        unsigned long sizes[] = {1, 16, 32, 64, 128, 256, 512, 1024};
        int n = sizeof(sizes) / sizeof(sizes[0]);
        for (int i = 0; i < n; i++)
        {
            void* p = heap_malloc(sizes[i]);
            TEST_ASSERT("slab alloc non-null", p != NULL);
            TEST_ASSERT("slab 16-byte aligned", ((unsigned long)p & 0xF) == 0);
            heap_free(p);
        }
    }
    TEST_PASS("basic alloc/free all classes");

    // LIFO reuse: free then re-alloc same size returns same pointer
    {
        void* p1 = heap_malloc(64);
        heap_free(p1);
        void* p2 = heap_malloc(64);
        TEST_ASSERT("slab LIFO reuse", p2 == p1);
        heap_free(p2);
    }
    TEST_PASS("LIFO reuse");

    // all slots distinct within a size class
    {
        void* ptrs[64];
        for (int i = 0; i < 64; i++)
        {
            ptrs[i] = heap_malloc(32);
            TEST_ASSERT("slab 64x alloc", ptrs[i] != NULL);
        }
        int distinct = 1;
        for (int i = 0; i < 64 && distinct; i++)
            for (int j = i + 1; j < 64 && distinct; j++)
                if (ptrs[i] == ptrs[j])
                    distinct = 0;
        TEST_ASSERT("slab all 64 distinct", distinct);
        for (int i = 0; i < 64; i++)
            heap_free(ptrs[i]);
    }
    TEST_PASS("64 allocs distinct");

    // data isolation between adjacent slab slots
    {
        unsigned char* a = (unsigned char*)heap_malloc(64);
        unsigned char* b = (unsigned char*)heap_malloc(64);
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
        TEST_ASSERT("slab block a intact", a_ok);
        TEST_ASSERT("slab block b intact", b_ok);
        heap_free(a);
        heap_free(b);
    }
    TEST_PASS("data isolation");

    // slab_owns correctly identifies slab pointers
    {
        void* slab_ptr = heap_malloc(64);
        void* heap_ptr = heap_malloc(4096);  // > 1024, goes to first-fit
        TEST_ASSERT("slab_owns slab ptr", slab_owns(slab_ptr) == 1);
        TEST_ASSERT("slab_owns heap ptr", slab_owns(heap_ptr) == 0);
        TEST_ASSERT("slab_owns null", slab_owns(NULL) == 0);
        heap_free(slab_ptr);
        heap_free(heap_ptr);
    }
    TEST_PASS("slab_owns dispatch");

    // cross-class allocations don't interfere
    {
        unsigned char* p16 = (unsigned char*)heap_malloc(16);
        unsigned char* p64 = (unsigned char*)heap_malloc(64);
        unsigned char* p256 = (unsigned char*)heap_malloc(256);
        unsigned char* p1024 = (unsigned char*)heap_malloc(1024);
        memset(p16, 0x11, 16);
        memset(p64, 0x22, 64);
        memset(p256, 0x33, 256);
        memset(p1024, 0x44, 1024);
        int ok = 1;
        for (int i = 0; i < 16; i++)
            if (p16[i] != 0x11)
                ok = 0;
        for (int i = 0; i < 64; i++)
            if (p64[i] != 0x22)
                ok = 0;
        for (int i = 0; i < 256; i++)
            if (p256[i] != 0x33)
                ok = 0;
        for (int i = 0; i < 1024; i++)
            if (p1024[i] != 0x44)
                ok = 0;
        TEST_ASSERT("cross-class data intact", ok);
        heap_free(p16);
        heap_free(p64);
        heap_free(p256);
        heap_free(p1024);
    }
    TEST_PASS("cross-class isolation");

    // slab grows automatically when a page fills up
    // 16-byte class: ~254 slots per page (4096 - header) / 16
    {
        void* ptrs[300];
        int count = 0;
        for (int i = 0; i < 300; i++)
        {
            ptrs[i] = heap_malloc(16);
            if (ptrs[i] != NULL)
                count++;
        }
        TEST_ASSERT("slab auto-grow 300 allocs", count == 300);
        for (int i = 0; i < 300; i++)
            heap_free(ptrs[i]);
    }
    TEST_PASS("auto-grow beyond one page");

    // rapid alloc/free cycle (stress)
    {
        for (int i = 0; i < 500; i++)
        {
            void* p = heap_malloc(128);
            TEST_ASSERT("slab rapid alloc", p != NULL);
            *(volatile unsigned long*)p = 0xCAFEBABEUL;
            TEST_ASSERT("slab rapid canary", *(volatile unsigned long*)p == 0xCAFEBABEUL);
            heap_free(p);
        }
    }
    TEST_PASS("rapid cycle x500");

    // fill-and-drain: alloc many, free all, alloc again
    {
        void* ptrs[128];
        for (int i = 0; i < 128; i++)
        {
            ptrs[i] = heap_malloc(64);
            TEST_ASSERT("slab fill alloc", ptrs[i] != NULL);
        }
        for (int i = 127; i >= 0; i--)
            heap_free(ptrs[i]);

        // After draining, allocator should still work
        void* p = heap_malloc(64);
        TEST_ASSERT("slab post-drain alloc", p != NULL);
        heap_free(p);
    }
    TEST_PASS("fill-and-drain 128");

    // mixed slab + first-fit: interleaved small/large allocations
    {
        void* s1 = heap_malloc(16);
        void* l1 = heap_malloc(4096);
        void* s2 = heap_malloc(512);
        void* l2 = heap_malloc(8192);
        void* s3 = heap_malloc(1024);
        TEST_ASSERT("mixed s1", s1 != NULL);
        TEST_ASSERT("mixed l1", l1 != NULL);
        TEST_ASSERT("mixed s2", s2 != NULL);
        TEST_ASSERT("mixed l2", l2 != NULL);
        TEST_ASSERT("mixed s3", s3 != NULL);
        heap_free(s2);
        heap_free(l1);
        heap_free(s1);
        heap_free(l2);
        heap_free(s3);
    }
    TEST_PASS("mixed slab+first-fit");

    // boundary: size exactly at each class boundary
    {
        unsigned long exact[] = {16, 32, 64, 128, 256, 512, 1024};
        int n = sizeof(exact) / sizeof(exact[0]);
        void* ptrs[7];
        for (int i = 0; i < n; i++)
        {
            ptrs[i] = heap_malloc(exact[i]);
            TEST_ASSERT("exact class alloc", ptrs[i] != NULL);
            memset(ptrs[i], (unsigned char)(i + 1), exact[i]);
        }
        int ok = 1;
        for (int i = 0; i < n; i++)
        {
            unsigned char* cp = (unsigned char*)ptrs[i];
            for (unsigned long j = 0; j < exact[i]; j++)
                if (cp[j] != (unsigned char)(i + 1))
                    ok = 0;
        }
        TEST_ASSERT("exact class data intact", ok);
        for (int i = 0; i < n; i++)
            heap_free(ptrs[i]);
    }
    TEST_PASS("exact class boundaries");

    // boundary: size one byte above each class → promoted to next class
    {
        unsigned long above[] = {17, 33, 65, 129, 257, 513};
        int n = sizeof(above) / sizeof(above[0]);
        void* ptrs[6];
        for (int i = 0; i < n; i++)
        {
            ptrs[i] = heap_malloc(above[i]);
            TEST_ASSERT("above-class alloc", ptrs[i] != NULL);
            memset(ptrs[i], 0xDD, above[i]);
        }
        int ok = 1;
        for (int i = 0; i < n; i++)
        {
            unsigned char* cp = (unsigned char*)ptrs[i];
            for (unsigned long j = 0; j < above[i]; j++)
                if (cp[j] != 0xDD)
                    ok = 0;
        }
        TEST_ASSERT("above-class data intact", ok);
        for (int i = 0; i < n; i++)
            heap_free(ptrs[i]);
    }
    TEST_PASS("class promotion");

    TEST_SUITE_END("Slab Allocator");
}
