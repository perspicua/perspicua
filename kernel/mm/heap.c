/*
 * heap.c - Hybrid kernel dynamic memory allocator.
 */

#include "mm/heap.h"

#include <stddef.h>

#include "stdio.h"

#include "core/lock.h"
#include "mm/failinject.h"
#include "mm/pmm.h"
#include "mm/slab.h"
#include "panic.h"

#define HEAP_SLAB_MAX  1024
#define HEAP_MAX_ALLOC (8 * 1024 * 1024)
#define HEAP_ALIGN(x)  (((x) + 15) & ~15UL)

// magic is checked before size is trusted, so a pointer this heap never returned cannot steer it.
struct heap_block_header {
    unsigned long size;
    struct heap_block_header *next;
    unsigned int magic;
    unsigned char is_free;
} __attribute__((aligned(16)));

#define HEAP_REDZONE_MAGIC 0xDEADBEEF
#define HEAP_MAGIC_ALLOC   0x48454150U // "HEAP"
#define HEAP_MAGIC_FREE    0x46524545U // "FREE"

#define HEAP_HEADER_SIZE sizeof(struct heap_block_header)

_Static_assert(sizeof(struct heap_block_header) == 32, "heap header must stay 32 bytes");

struct heap_block_footer {
    unsigned int magic;
} __attribute__((aligned(16)));

#define HEAP_FOOTER_SIZE sizeof(struct heap_block_footer)

static struct heap_block_header *heap_free_list = NULL;
static spinlock_t heap_lock = SPINLOCK_INIT;
static unsigned long heap_total_size = 0;

static struct heap_block_header *heap_expand(unsigned long min_size)
{
    // The footer too, or the block cannot satisfy the request and heap_malloc spins.
    unsigned long total = min_size + HEAP_HEADER_SIZE + HEAP_FOOTER_SIZE;
    unsigned long pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;

    // Rounded as pmm rounds, so the block's tail stays the heap's and is not stranded.
    unsigned long rounded = 1;
    while (rounded < pages) {
        rounded <<= 1;
        if (rounded == 0) {
            return NULL;
        }
    }
    pages = rounded;

    void *region = pmm_alloc_pages(pages);
    if (!region) {
        return NULL;
    }

    struct heap_block_header *block = (struct heap_block_header *)region;
    block->size = pages * PAGE_SIZE - HEAP_HEADER_SIZE;
    block->next = NULL;
    block->is_free = 1;
    block->magic = HEAP_MAGIC_FREE;

    // heap_expand runs outside heap_lock, so two cores can grow concurrently.
    __atomic_fetch_add(&heap_total_size, pages * PAGE_SIZE, __ATOMIC_RELAXED);

    return block;
}

// Address order, so coalescing only has to look at the neighbours.
static void heap_insert_free(struct heap_block_header *block)
{
    block->is_free = 1;
    block->magic = HEAP_MAGIC_FREE;

    struct heap_block_header *prev = NULL;
    struct heap_block_header *next = heap_free_list;
    while (next && (unsigned long)next < (unsigned long)block) {
        prev = next;
        next = next->next;
    }

    block->next = next;
    if (prev) {
        prev->next = block;
    } else {
        heap_free_list = block;
    }

    if (next && next->is_free) {
        unsigned char *block_end = (unsigned char *)block + HEAP_HEADER_SIZE + block->size;
        if (block_end == (unsigned char *)next) {
            block->size += HEAP_HEADER_SIZE + next->size;
            block->next = next->next;
        }
    }

    if (prev && prev->is_free) {
        unsigned char *prev_end = (unsigned char *)prev + HEAP_HEADER_SIZE + prev->size;
        if (prev_end == (unsigned char *)block) {
            prev->size += HEAP_HEADER_SIZE + block->size;
            prev->next = block->next;
        }
    }
}

void heap_init(void)
{
    slab_init();

    heap_free_list = heap_expand(PAGE_SIZE);
    if (!heap_free_list) {
        PANIC("heap: failed to initialize pool");
    }

    pr_info("heap: %lu bytes aligned to 16 bytes\n", heap_free_list->size);
}

#ifdef CONFIG_TESTS
static struct fail_arming heap_fail;

unsigned long heap_test_fail_nth(unsigned long n)
{
    return fail_arm(&heap_fail, n);
}
#endif

void *heap_malloc(unsigned long size)
{
    if (size == 0 || size > HEAP_MAX_ALLOC) {
        return NULL;
    }

#ifdef CONFIG_TESTS
    if (fail_fire(&heap_fail)) {
        return NULL;
    }
#endif

    if (size <= HEAP_SLAB_MAX) {
        return slab_alloc(size);
    }

    size = HEAP_ALIGN(size);

    // The redzone footer is carved from the block, so a fit covers it too.
    unsigned long need = size + HEAP_FOOTER_SIZE;

    while (1) {
        unsigned long flags = spin_lock_irqsave(&heap_lock);

        struct heap_block_header *prev = NULL;
        struct heap_block_header *curr = heap_free_list;
        while (curr) {
            if (curr->size >= need) {
                if (prev) {
                    prev->next = curr->next;
                } else {
                    heap_free_list = curr->next;
                }

                if (curr->size >= need + HEAP_HEADER_SIZE + 16) {
                    struct heap_block_header *split =
                        (struct heap_block_header *)((unsigned char *)curr + HEAP_HEADER_SIZE
                                                     + need);
                    split->size = curr->size - need - HEAP_HEADER_SIZE;
                    split->is_free = 1;
                    split->next = NULL;

                    heap_insert_free(split);
                    curr->size = size;
                } else {
                    curr->size -= HEAP_FOOTER_SIZE;
                }

                struct heap_block_footer *footer =
                    (struct heap_block_footer *)((unsigned char *)curr + HEAP_HEADER_SIZE
                                                 + curr->size);
                footer->magic = HEAP_REDZONE_MAGIC;
                curr->is_free = 0;
                curr->magic = HEAP_MAGIC_ALLOC;
                curr->next = NULL;

                spin_unlock_irqrestore(&heap_lock, flags);
                return (void *)((unsigned char *)curr + HEAP_HEADER_SIZE);
            }
            prev = curr;
            curr = curr->next;
        }

        spin_unlock_irqrestore(&heap_lock, flags);

        struct heap_block_header *new_block = heap_expand(size);
        if (!new_block) {
            return NULL;
        }

        flags = spin_lock_irqsave(&heap_lock);
        heap_insert_free(new_block);
        spin_unlock_irqrestore(&heap_lock, flags);
    }
}

void heap_free(void *ptr)
{
    if (!ptr) {
        return;
    }

    if (slab_owns(ptr)) {
        slab_free(ptr);
        return;
    }

    struct heap_block_header *block =
        (struct heap_block_header *)((unsigned char *)ptr - HEAP_HEADER_SIZE);

    if (!pmm_is_managed((void *)block)) {
        PANIC("heap: pointer was not returned by heap_malloc");
    }

    unsigned long flags = spin_lock_irqsave(&heap_lock);

    // Confirm the header is ours before size is used to locate anything.
    if (block->magic == HEAP_MAGIC_FREE) {
        spin_unlock_irqrestore(&heap_lock, flags);
        PANIC("heap: double free detected");
    }
    if (block->magic != HEAP_MAGIC_ALLOC) {
        spin_unlock_irqrestore(&heap_lock, flags);
        PANIC("heap: pointer was not returned by heap_malloc");
    }

    struct heap_block_footer *footer =
        (struct heap_block_footer *)((unsigned char *)block + HEAP_HEADER_SIZE + block->size);
    if (footer->magic != HEAP_REDZONE_MAGIC) {
        spin_unlock_irqrestore(&heap_lock, flags);
        PANIC("heap: buffer overflow detected (redzone corrupted)");
    }

    block->size += HEAP_FOOTER_SIZE;
    heap_insert_free(block);

    spin_unlock_irqrestore(&heap_lock, flags);
}

unsigned long heap_get_total(void)
{
    return heap_total_size + slab_get_total();
}

#ifdef CONFIG_TESTS
int heap_test_is_tagged_allocated(const void *ptr)
{
    if (!ptr || slab_owns((void *)ptr)) {
        return 0;
    }

    const struct heap_block_header *block =
        (const struct heap_block_header *)((const unsigned char *)ptr - HEAP_HEADER_SIZE);
    return block->magic == HEAP_MAGIC_ALLOC;
}

unsigned long heap_test_usable_size(const void *ptr)
{
    if (!ptr) {
        return 0;
    }

    if (slab_owns((void *)ptr)) {
        return slab_test_object_size((void *)ptr);
    }

    const struct heap_block_header *block =
        (const struct heap_block_header *)((const unsigned char *)ptr - HEAP_HEADER_SIZE);
    return block->size;
}

int heap_test_redzone_ok(const void *ptr)
{
    if (!ptr) {
        return 0;
    }

    // Slab objects have no footer: their free canary is checked when slab_alloc hands them out.
    if (slab_owns((void *)ptr)) {
        return 1;
    }

    const struct heap_block_header *block =
        (const struct heap_block_header *)((const unsigned char *)ptr - HEAP_HEADER_SIZE);
    if (block->magic != HEAP_MAGIC_ALLOC) {
        return 0;
    }

    const struct heap_block_footer *footer =
        (const struct heap_block_footer *)((const unsigned char *)block + HEAP_HEADER_SIZE
                                           + block->size);
    return footer->magic == HEAP_REDZONE_MAGIC;
}
#endif
