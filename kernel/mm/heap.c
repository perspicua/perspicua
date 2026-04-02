/*
 * heap.c - Implementation of the kernel heap allocator.
 *
 * This module manages dynamic memory using a hybrid fast-slab and first-fit
 * buddy fallback. It uses contiguous PMM pages to grow the first-fit pool.
 */

#include "mm/heap.h"

#include "stdio.h"
#include "panic.h"

#include "core/lock.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "mm/pmm.h"

#define HEAP_SLAB_MAX  1024
#define HEAP_MAX_ALLOC (8 * 1024 * 1024)
#define HEAP_ALIGN(x)  (((x) + 15) & ~15UL)

/*
 * struct heap_block_header - Prefix for first-fit pool allocations.
 */
struct heap_block_header {
    unsigned long size;
    struct heap_block_header *next;
    unsigned char is_free;
} __attribute__((aligned(16)));

#define HEAP_REDZONE_MAGIC 0xDEADBEEF

#define HEAP_HEADER_SIZE sizeof(struct heap_block_header)

struct heap_block_footer {
    unsigned int magic;
} __attribute__((aligned(16)));

#define HEAP_FOOTER_SIZE sizeof(struct heap_block_footer)

static struct heap_block_header *heap_free_list = NULL;
static spinlock_t heap_lock = SPINLOCK_INIT;
static unsigned long heap_total_size = 0;
static unsigned long heap_used_size = 0;

/*
 * heap_expand - Requests contiguous pages from PMM to grow the pool.
 */
static struct heap_block_header *heap_expand(unsigned long min_size)
{
    unsigned long total = min_size + HEAP_HEADER_SIZE;
    unsigned long pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;

    void *region = pmm_alloc_pages(pages);
    if (!region) {
        return NULL;
    }

    struct heap_block_header *block = (struct heap_block_header *)region;
    block->size = pages * PAGE_SIZE - HEAP_HEADER_SIZE;
    block->next = NULL;
    block->is_free = 1;

    heap_total_size += pages * PAGE_SIZE;

    return block;
}

/*
 * heap_insert_free - Inserts a block into the address-ordered free list.
 *
 * Address ordering allows for O(n) coalescing by checking only the
 * immediate predecessor and successor in the list.
 */
static void heap_insert_free(struct heap_block_header *block)
{
    block->is_free = 1;

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

    /* Coalesce with successor */
    if (next && next->is_free) {
        unsigned char *block_end = (unsigned char *)block + HEAP_HEADER_SIZE + block->size;
        if (block_end == (unsigned char *)next) {
            block->size += HEAP_HEADER_SIZE + next->size;
            block->next = next->next;
        }
    }

    /* Coalesce with predecessor */
    if (prev && prev->is_free) {
        unsigned char *prev_end = (unsigned char *)prev + HEAP_HEADER_SIZE + prev->size;
        if (prev_end == (unsigned char *)block) {
            prev->size += HEAP_HEADER_SIZE + block->size;
            prev->next = block->next;
        }
    }
}

/*
 * heap_init - Initializes slab layer and the first-fit pool.
 */
void heap_init(void)
{
    slab_init();

    heap_free_list = heap_expand(PAGE_SIZE);
    if (!heap_free_list) {
        PANIC("heap: failed to initialize pool");
    }

    pr_info("heap: %lu bytes aligned to 16 bytes\n", heap_free_list->size);
}

/*
 * heap_malloc - Dispatches to slab for small objects or uses first-fit search.
 */
void *heap_malloc(unsigned long size)
{
    if (size == 0 || size > HEAP_MAX_ALLOC) {
        return NULL;
    }

    if (size <= HEAP_SLAB_MAX) {
        return slab_alloc(size);
    }

    size = HEAP_ALIGN(size);

    while (1) {
        unsigned long flags = spin_lock_irqsave(&heap_lock);

        struct heap_block_header *prev = NULL;
        struct heap_block_header *curr = heap_free_list;
        while (curr) {
            if (curr->size >= size + HEAP_FOOTER_SIZE) {
                if (prev) {
                    prev->next = curr->next;
                } else {
                    heap_free_list = curr->next;
                }

                /* Split if remainder is large enough for a header and usable space */
                if (curr->size >= size + HEAP_FOOTER_SIZE + HEAP_HEADER_SIZE + 16) {
                    struct heap_block_header *split =
                        (struct heap_block_header *)((unsigned char *)curr + HEAP_HEADER_SIZE + size
                                                     + HEAP_FOOTER_SIZE);
                    split->size = curr->size - size - HEAP_FOOTER_SIZE - HEAP_HEADER_SIZE;
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
                curr->next = NULL;
                heap_used_size += curr->size + HEAP_HEADER_SIZE;

                spin_unlock_irqrestore(&heap_lock, flags);
                return (void *)((unsigned char *)curr + HEAP_HEADER_SIZE);
            }
            prev = curr;
            curr = curr->next;
        }

        spin_unlock_irqrestore(&heap_lock, flags);

        struct heap_block_header *new_block = heap_expand(size + HEAP_FOOTER_SIZE);
        if (!new_block) {
            return NULL;
        }

        flags = spin_lock_irqsave(&heap_lock);
        heap_insert_free(new_block);
        spin_unlock_irqrestore(&heap_lock, flags);
    }
}

/*
 * heap_free - Returns memory to pool; detects double-frees in first-fit pool.
 */
void heap_free(void *ptr)
{
    if (!ptr) {
        return;
    }

    if (slab_owns(ptr)) {
        slab_free(ptr);
        return;
    }

    unsigned long flags = spin_lock_irqsave(&heap_lock);
    struct heap_block_header *block =
        (struct heap_block_header *)((unsigned char *)ptr - HEAP_HEADER_SIZE);

    struct heap_block_footer *footer =
        (struct heap_block_footer *)((unsigned char *)block + HEAP_HEADER_SIZE + block->size);
    if (footer->magic != HEAP_REDZONE_MAGIC) {
        PANIC("heap: buffer overflow detected (redzone corrupted)");
    }

    if (block->is_free) {
        PANIC("heap: double free detected");
    }

    heap_used_size -= block->size + HEAP_HEADER_SIZE;
    block->size += HEAP_FOOTER_SIZE;
    heap_insert_free(block);

    spin_unlock_irqrestore(&heap_lock, flags);
}

/*
 * heap_get_used - Returns combined usage from both layers.
 */
unsigned long heap_get_used(void)
{
    return heap_used_size + slab_get_used();
}

/*
 * heap_get_total - Returns combined total from both layers.
 */
unsigned long heap_get_total(void)
{
    return heap_total_size + slab_get_total();
}
