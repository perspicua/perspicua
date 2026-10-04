/*
 * test_heap_props.c - Allocator properties held across a sweep of sizes.
 *
 * Where test_heap.c asks for sizes chosen by hand, every assertion here runs
 * against every size in the sweep, so a property that only breaks at one
 * boundary still fails.
 */

#include <stddef.h>

#include "test.h"

#include "string.h"

#include "mm/heap.h"
#include "mm/slab.h"

// Mirrors of heap.c's internals, which a test may not include.
#define TEST_SLAB_MAX    1024 // HEAP_SLAB_MAX: at or below this, slab serves it
#define TEST_ALIGN       16   // HEAP_ALIGN quantum
#define TEST_HEADER_SIZE 32   // sizeof(struct heap_block_header)
#define TEST_FOOTER_SIZE 16   // sizeof(struct heap_block_footer)

// Remainder below this stays with the block instead of becoming a free block.
#define TEST_SPLIT_MIN (TEST_HEADER_SIZE + 16)

#define MAX_SWEEP 128

// Allocations made to find three that sit back to back.
#define HOLE_RUN 8

static unsigned long sweep[MAX_SWEEP];
static int sweep_count;
static int sweep_dropped;

#define GUARD_BYTE 0x5A

static void sweep_add(unsigned long n)
{
    if (n == 0) {
        return;
    }
    for (int i = 0; i < sweep_count; i++) {
        if (sweep[i] == n) {
            return;
        }
    }
    if (sweep_count >= MAX_SWEEP) {
        sweep_dropped = 1;
        return;
    }
    sweep[sweep_count++] = n;
}

// Dense where the allocator changes behaviour, thin everywhere else.
static void sweep_build(void)
{
    static const unsigned long classes[] = {16, 32, 64, 128, 256, 512, 1024};

    sweep_count = 0;
    sweep_dropped = 0;

    for (unsigned long n = 1; n <= 2 * TEST_ALIGN; n++) {
        sweep_add(n);
    }

    for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); i++) {
        sweep_add(classes[i] - 1);
        sweep_add(classes[i]);
        sweep_add(classes[i] + 1);
    }

    for (unsigned long n = TEST_SLAB_MAX - 2; n <= TEST_SLAB_MAX + 2; n++) {
        sweep_add(n);
    }

    sweep_add(2047);
    sweep_add(2048);
    sweep_add(2049);
    sweep_add(4095);
    sweep_add(4096);
    sweep_add(4097);
    sweep_add(8192);
}

// Depends on the request as well as the offset, so a byte written by one
// allocation cannot pass for the right byte of another.
static unsigned char fill_byte(unsigned long n, unsigned long i)
{
    return (unsigned char)(0xC0 ^ (n * 31 + i));
}

void test_heap_props(void)
{
    TEST_SUITE_BEGIN("Heap Properties");

    sweep_build();
    TEST_ASSERT("sweep fits its table", sweep_count > 0 && !sweep_dropped);

    // routing: the slab path has no footer and the first-fit path does, so
    // every assertion below reads differently on the wrong side of 1024
    {
        int routing_ok = 1;

        for (int i = 0; i < sweep_count; i++) {
            unsigned long n = sweep[i];
            void *p = heap_malloc(n);
            if (!p) {
                routing_ok = 0;
                continue;
            }

            if (n <= TEST_SLAB_MAX) {
                if (!slab_owns(p)) {
                    routing_ok = 0;
                }
            } else if (!heap_test_is_tagged_allocated(p)) {
                routing_ok = 0;
            }

            heap_free(p);
        }

        TEST_ASSERT("every size is served by the allocator its size selects", routing_ok);
    }

    // usable size never falls below the request
    {
        int usable_ok = 1;
        int alignment_ok = 1;

        for (int i = 0; i < sweep_count; i++) {
            unsigned long n = sweep[i];
            void *p = heap_malloc(n);
            if (!p) {
                usable_ok = 0;
                continue;
            }

            if (heap_test_usable_size(p) < n) {
                usable_ok = 0;
            }
            if (((unsigned long)p & (TEST_ALIGN - 1)) != 0) {
                alignment_ok = 0;
            }

            heap_free(p);
        }

        TEST_ASSERT("usable size covers every request", usable_ok);
        TEST_ASSERT("every request is aligned", alignment_ok);
    }

    // exactly the requested bytes survive a write
    {
        int exact_write_ok = 1;

        for (int i = 0; i < sweep_count; i++) {
            unsigned long n = sweep[i];
            unsigned char *p = heap_malloc(n);
            if (!p) {
                exact_write_ok = 0;
                continue;
            }

            for (unsigned long b = 0; b < n; b++) {
                p[b] = fill_byte(n, b);
            }
            for (unsigned long b = 0; b < n; b++) {
                if (p[b] != fill_byte(n, b)) {
                    exact_write_ok = 0;
                    break;
                }
            }

            heap_free(p);
        }

        TEST_ASSERT("exactly the requested bytes survive a write", exact_write_ok);
    }

    // the usable region is writable to its last byte: one further and the
    // heap_free below would panic rather than return
    {
        int usable_write_ok = 1;
        int redzone_ok = 1;

        for (int i = 0; i < sweep_count; i++) {
            unsigned long n = sweep[i];
            unsigned char *p = heap_malloc(n);
            if (!p) {
                usable_write_ok = 0;
                continue;
            }

            unsigned long usable = heap_test_usable_size(p);
            for (unsigned long b = 0; b < usable; b++) {
                p[b] = fill_byte(usable, b);
            }
            for (unsigned long b = 0; b < usable; b++) {
                if (p[b] != fill_byte(usable, b)) {
                    usable_write_ok = 0;
                    break;
                }
            }

            if (!heap_test_redzone_ok(p)) {
                redzone_ok = 0;
            }

            heap_free(p);
        }

        TEST_ASSERT("the whole usable region is writable", usable_write_ok);
        TEST_ASSERT("filling the usable region leaves the redzone intact", redzone_ok);
    }

    // a full allocation does not reach its neighbours. Three at a time,
    // because an overrun lands on whichever of them follows it in memory
    {
        int isolation_ok = 1;

        for (int i = 0; i < sweep_count; i++) {
            unsigned long n = sweep[i];

            unsigned char *before = heap_malloc(n);
            unsigned char *victim = heap_malloc(n);
            unsigned char *after = heap_malloc(n);

            if (!before || !victim || !after) {
                isolation_ok = 0;
                heap_free(before);
                heap_free(victim);
                heap_free(after);
                continue;
            }

            unsigned long before_usable = heap_test_usable_size(before);
            unsigned long after_usable = heap_test_usable_size(after);

            memset(before, GUARD_BYTE, before_usable);
            memset(after, GUARD_BYTE, after_usable);

            memset(victim, 0xFF, heap_test_usable_size(victim));

            for (unsigned long b = 0; b < before_usable; b++) {
                if (before[b] != GUARD_BYTE) {
                    isolation_ok = 0;
                    break;
                }
            }
            for (unsigned long b = 0; b < after_usable; b++) {
                if (after[b] != GUARD_BYTE) {
                    isolation_ok = 0;
                    break;
                }
            }

            if (!heap_test_redzone_ok(before) || !heap_test_redzone_ok(after)) {
                isolation_ok = 0;
            }

            heap_free(before);
            heap_free(victim);
            heap_free(after);
        }

        TEST_ASSERT("a full allocation does not reach its neighbours", isolation_ok);
    }

    // A block freed between two live ones cannot coalesce, and nothing below it
    // fits its size, so it is first fit for a request of that size or a little
    // less. Walking the request down crosses the point where the remainder is
    // big enough to split off.
    {
        const unsigned long base = 4000;
        const unsigned long stride = TEST_HEADER_SIZE + base + TEST_FOOTER_SIZE;
        const unsigned long hole_free = base + TEST_FOOTER_SIZE;
        unsigned char *run[HOLE_RUN];
        unsigned char *hole = NULL;

        for (int i = 0; i < HOLE_RUN; i++) {
            run[i] = heap_malloc(base);
        }

        for (int i = 0; i + 2 < HOLE_RUN && !hole; i++) {
            if (run[i] && run[i + 1] && run[i + 2] && run[i + 1] == run[i] + stride
                && run[i + 2] == run[i + 1] + stride) {
                hole = run[i + 1];
                run[i + 1] = NULL;
            }
        }

        TEST_ASSERT("three neighbours carved back to back", hole != NULL);

        if (hole) {
            heap_free(hole);

            int whole_ok = 0;
            int rule_ok = 1;
            int props_ok = 1;

            for (unsigned long take = 0; take <= 2 * TEST_SPLIT_MIN; take += TEST_ALIGN) {
                unsigned long want = base - take;
                unsigned char *p = heap_malloc(want);
                if (!p) {
                    props_ok = 0;
                    continue;
                }

                unsigned long usable = heap_test_usable_size(p);

                if (p == hole) {
                    unsigned long need = want + TEST_FOOTER_SIZE;
                    unsigned long expect =
                        hole_free >= need + TEST_SPLIT_MIN ? want : hole_free - TEST_FOOTER_SIZE;
                    if (usable != expect) {
                        rule_ok = 0;
                    }
                    if (take == 0 && usable == base) {
                        whole_ok = 1;
                    }
                }

                if (usable < want) {
                    props_ok = 0;
                }
                for (unsigned long b = 0; b < usable; b++) {
                    p[b] = fill_byte(want, b);
                }
                if (!heap_test_redzone_ok(p)) {
                    props_ok = 0;
                }

                heap_free(p);
            }

            TEST_ASSERT("a request the hole fits exactly takes it whole", whole_ok);
            TEST_ASSERT("the hole splits exactly when the remainder holds a block", rule_ok);
            TEST_ASSERT("every request carries its size and its redzone", props_ok);
        }

        for (int i = 0; i < HOLE_RUN; i++) {
            heap_free(run[i]);
        }
    }

    // the same sweep held live all at once: splitting and coalescing need a
    // populated pool, which an alloc/free pair on its own never builds
    {
        static void *live[MAX_SWEEP];
        int concurrent_ok = 1;
        int held = 0;

        for (int i = 0; i < sweep_count; i++) {
            live[i] = heap_malloc(sweep[i]);
            if (!live[i]) {
                concurrent_ok = 0;
                continue;
            }
            memset(live[i], (unsigned char)i, sweep[i]);
            held++;
        }

        TEST_ASSERT("the whole sweep fits in the heap at once", held == sweep_count);

        for (int i = 0; i < sweep_count; i++) {
            if (!live[i]) {
                continue;
            }

            const unsigned char *p = live[i];
            for (unsigned long b = 0; b < sweep[i]; b++) {
                if (p[b] != (unsigned char)i) {
                    concurrent_ok = 0;
                    break;
                }
            }
            if (!heap_test_redzone_ok(live[i])) {
                concurrent_ok = 0;
            }
        }

        // Freed in reverse so the coalescing path runs against a populated pool
        for (int i = sweep_count - 1; i >= 0; i--) {
            heap_free(live[i]);
            live[i] = NULL;
        }

        TEST_ASSERT("every live allocation keeps its own bytes", concurrent_ok);
    }

    TEST_SUITE_END("Heap Properties");
}
