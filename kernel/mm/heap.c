/*
 * heap.c - Implementation of the kernel heap allocator.
 *
 * This file manages dynamic memory allocation using a hybrid approach
 * with a fast slab layer for small objects and a first-fit buddy fallback
 * for larger requests.
 */

#include "mm/heap.h"

#include "mm/slab.h"
#include "mm/pmm.h"

#include "core/lock.h"
#include "stdio.h"
#include "panic.h"
#include "core/timer.h"

/* Allocations <= this value are handled by the slab allocator */
#define HEAP_SLAB_MAX 1024

/* Maximum size for a single heap allocation (8 MB) */
#define HEAP_MAX_ALLOC (8 * 1024 * 1024)

/* Block header preceding every allocation in the first-fit pool */
struct heap_block_header {
    unsigned long size;             /* Usable size excluding the header */
    struct heap_block_header *next; /* Next block in the free list */
    unsigned char is_free;          /* Flag indicating if block is free */
} __attribute__((aligned(16)));

#define HEAP_HEADER_SIZE sizeof(struct heap_block_header)
#define HEAP_ALIGN(x)    (((x) + 15) & ~15UL)

/* Heap state and synchronization */
static struct heap_block_header *heap_free_list = NULL;
static spinlock_t heap_lock = SPINLOCK_INIT;
static unsigned long heap_total_size = 0;
static unsigned long heap_used_size = 0;

/*
 * heap_expand - Requests contiguous pages from the PMM to grow the heap.
 * Returns a free block header for the new region, or NULL on failure.
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
 * heap_insert_free - Inserts a free block into the free list in address
 * order and coalesces it with any physically adjacent free neighbours.
 *
 * Keeping the list sorted by address is what makes O(n) coalescing correct:
 * a block can only be physically adjacent to its immediate predecessor and
 * successor in address order, so a single forward pass is sufficient.
 *
 * Must be called with heap_lock held.
 */
static void heap_insert_free(struct heap_block_header *block)
{
    block->is_free = 1;

    /* Find the insertion point: prev->next > block > prev */
    struct heap_block_header *prev = NULL;
    struct heap_block_header *next = heap_free_list;
    while (next && (unsigned long)next < (unsigned long)block) {
        prev = next;
        next = next->next;
    }

    /* Link block into the list */
    block->next = next;
    if (prev) {
        prev->next = block;
    } else {
        heap_free_list = block;
    }

    /*
     * Coalesce with successor: if block's usable region runs right up to
     * the start of next, merge them. Only merge if next is actually free.
     */
    if (next && next->is_free) {
        unsigned char *block_end = (unsigned char *)block + HEAP_HEADER_SIZE + block->size;
        if (block_end == (unsigned char *)next) {
            block->size += HEAP_HEADER_SIZE + next->size;
            block->next = next->next;
        }
    }

    /*
     * Coalesce with predecessor: if prev's usable region runs right up to
     * the start of block, merge them. Only merge if prev is actually free.
     */
    if (prev && prev->is_free) {
        unsigned char *prev_end = (unsigned char *)prev + HEAP_HEADER_SIZE + prev->size;
        if (prev_end == (unsigned char *)block) {
            prev->size += HEAP_HEADER_SIZE + block->size;
            prev->next = block->next;
        }
    }
}

/*
 * heap_init - Boot-time initialization of all heap layers.
 */
void heap_init(void)
{
    slab_init();

    heap_free_list = heap_expand(PAGE_SIZE);
    if (!heap_free_list) {
        PANIC("HEAP: Failed to initialize pool.");
    }

    pr_info("heap: %lu bytes aligned to 16 bytes\n", heap_free_list->size);
}

/*
 * heap_malloc - Allocates a memory block from the appropriate layer.
 * Returns NULL on failure; never panics.
 */
void *heap_malloc(unsigned long size)
{
    if (size == 0 || size > HEAP_MAX_ALLOC) {
        return NULL;
    }

    /* Use the O(1) slab path for small objects */
    if (size <= HEAP_SLAB_MAX) {
        return slab_alloc(size);
    }

    size = HEAP_ALIGN(size);

    while (1) {
        unsigned long flags = spin_lock_irqsave(&heap_lock);

        /* First-fit search in the free list */
        struct heap_block_header *prev = NULL;
        struct heap_block_header *curr = heap_free_list;
        while (curr) {
            if (curr->size >= size) {
                /* Found a suitable block. Unlink it from the free list. */
                if (prev) {
                    prev->next = curr->next;
                } else {
                    heap_free_list = curr->next;
                }

                /* Split if there is enough room for a usable remainder */
                if (curr->size >= size + HEAP_HEADER_SIZE + 16) {
                    struct heap_block_header *split =
                        (struct heap_block_header *)((unsigned char *)curr + HEAP_HEADER_SIZE
                                                     + size);
                    split->size = curr->size - size - HEAP_HEADER_SIZE;
                    split->is_free = 1;
                    split->next = NULL;

                    /* Re-insert the remainder back into the free list */
                    heap_insert_free(split);

                    curr->size = size;
                }

                curr->is_free = 0;
                curr->next = NULL;
                heap_used_size += curr->size + HEAP_HEADER_SIZE;

                spin_unlock_irqrestore(&heap_lock, flags);
                return (void *)((unsigned char *)curr + HEAP_HEADER_SIZE);
            }
            prev = curr;
            curr = curr->next;
        }

        /* No suitable block found — drop lock and expand the heap */
        spin_unlock_irqrestore(&heap_lock, flags);

        struct heap_block_header *new_block = heap_expand(size);
        if (!new_block) {
            return NULL;
        }

        /* Re-lock and insert the new region. It will be found in the next iteration. */
        flags = spin_lock_irqsave(&heap_lock);
        heap_insert_free(new_block);
        spin_unlock_irqrestore(&heap_lock, flags);
    }
}

/*
 * heap_free - Returns memory to the pool.
 */
void heap_free(void *ptr)
{
    if (!ptr) {
        return;
    }

    /* Redirect to slab if it owns this pointer */
    if (slab_owns(ptr)) {
        slab_free(ptr);
        return;
    }

    unsigned long flags = spin_lock_irqsave(&heap_lock);
    struct heap_block_header *block =
        (struct heap_block_header *)((unsigned char *)ptr - HEAP_HEADER_SIZE);

    if (block->is_free) {
        PANIC("HEAP: Double free detected.");
    }

    heap_used_size -= block->size + HEAP_HEADER_SIZE;
    heap_insert_free(block);

    spin_unlock_irqrestore(&heap_lock, flags);
}

/*
 * heap_get_used - Combined used memory count.
 */
unsigned long heap_get_used(void)
{
    return heap_used_size + slab_get_used();
}

/*
 * heap_get_total - Combined total memory count.
 */
unsigned long heap_get_total(void)
{
    return heap_total_size + slab_get_total();
}
