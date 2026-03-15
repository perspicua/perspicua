/*
 * heap.c - Implementation of the kernel heap allocator.
 *
 * This file manages dynamic memory allocation using a hybrid approach
 * with a fast slab layer for small objects and a first-fit buddy fallback
 * for larger requests.
 */

#include "heap.h"

#include "slab.h"
#include "lock.h"
#include "pmm.h"
#include "stdio.h"
#include "panic.h"
#include "timer.h"

/* Allocations <= this value are handled by the slab allocator */
#define HEAP_SLAB_MAX 1024

/* Block header preceding every allocation in the first-fit pool */
struct heap_block_header
{
    unsigned long size;             /* Usable size excluding the header */
    struct heap_block_header* next; /* Next block in the free list */
    unsigned char is_free;          /* Flag indicating if block is free */
} __attribute__((aligned(16)));

#define HEAP_HEADER_SIZE sizeof(struct heap_block_header)
#define HEAP_ALIGN(x) (((x) + 15) & ~15UL)

/* Heap state and synchronization */
static struct heap_block_header* heap_free_list = (void*)0;
static spinlock_t heap_lock = SPINLOCK_INIT;
static unsigned long heap_total_size = 0;
static unsigned long heap_used_size = 0;

/*
 * heap_expand - Requests contiguous pages from the PMM to grow the heap.
 */
static struct heap_block_header* heap_expand(unsigned long min_size)
{
    unsigned long total = min_size + HEAP_HEADER_SIZE;
    unsigned long pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;

    void* region = pmm_alloc_pages(pages);
    if (!region)
    {
        return (void*)0;
    }

    struct heap_block_header* block = (struct heap_block_header*)region;
    block->size = pages * PAGE_SIZE - HEAP_HEADER_SIZE;
    block->next = (void*)0;
    block->is_free = 1;

    heap_total_size += pages * PAGE_SIZE;

    return block;
}

/*
 * heap_init - Boot-time initialization of all heap layers.
 */
void heap_init(void)
{
    slab_init();

    heap_free_list = heap_expand(PAGE_SIZE);
    if (!heap_free_list)
    {
        PANIC("HEAP: Failed to initialize pool.");
    }

    printf("[ HEAP ] First-fit pool: %lu bytes\n", heap_free_list->size);
    printf("[ HEAP ] Alignment: 16 bytes\n");
}

/*
 * heap_malloc - Allocates a memory block from the appropriate layer.
 */
void* heap_malloc(unsigned long size)
{
    if (size == 0)
    {
        return (void*)0;
    }

    /* Use the O(1) slab path for small objects */
    if (size <= HEAP_SLAB_MAX)
    {
        return slab_alloc(size);
    }

    /* Fall back to first-fit search for large objects */
    unsigned long flags = spin_lock_irqsave(&heap_lock);
    size = HEAP_ALIGN(size);

    struct heap_block_header* curr = heap_free_list;
    while (curr)
    {
        if (curr->is_free && curr->size >= size)
        {
            /* Split the block if there is enough remaining space */
            if (curr->size >= size + HEAP_HEADER_SIZE + 16)
            {
                struct heap_block_header* new_block =
                    (struct heap_block_header*)((unsigned char*)curr + HEAP_HEADER_SIZE + size);
                new_block->size = curr->size - size - HEAP_HEADER_SIZE;
                new_block->next = curr->next;
                new_block->is_free = 1;

                curr->size = size;
                curr->next = new_block;
            }

            curr->is_free = 0;
            heap_used_size += curr->size + HEAP_HEADER_SIZE;
            spin_unlock_irqrestore(&heap_lock, flags);
            return (void*)((unsigned char*)curr + HEAP_HEADER_SIZE);
        }
        curr = curr->next;
    }

    /* No suitable block found; expand the heap */
    struct heap_block_header* new_page = heap_expand(size);
    if (!new_page)
    {
        PANIC("HEAP: Out of memory expansion.");
    }

    /* Re-insert in address order to maintain address space consistency */
    if (!heap_free_list || (unsigned long)new_page < (unsigned long)heap_free_list)
    {
        new_page->next = heap_free_list;
        heap_free_list = new_page;
    }
    else
    {
        struct heap_block_header* scan = heap_free_list;
        while (scan->next && (unsigned long)scan->next < (unsigned long)new_page)
        {
            scan = scan->next;
        }
        new_page->next = scan->next;
        scan->next = new_page;
    }

    /* Allocate directly from the newly added space */
    if (new_page->size >= size + HEAP_HEADER_SIZE + 16)
    {
        struct heap_block_header* split =
            (struct heap_block_header*)((unsigned char*)new_page + HEAP_HEADER_SIZE + size);
        split->size = new_page->size - size - HEAP_HEADER_SIZE;
        split->next = new_page->next;
        split->is_free = 1;

        new_page->size = size;
        new_page->next = split;
    }

    new_page->is_free = 0;
    heap_used_size += new_page->size + HEAP_HEADER_SIZE;
    spin_unlock_irqrestore(&heap_lock, flags);
    return (void*)((unsigned char*)new_page + HEAP_HEADER_SIZE);
}

/*
 * heap_free - Returns memory to the pool.
 */
void heap_free(void* ptr)
{
    if (!ptr)
    {
        return;
    }

    /* Redirect to slab if it owns this pointer */
    if (slab_owns(ptr))
    {
        slab_free(ptr);
        return;
    }

    unsigned long flags = spin_lock_irqsave(&heap_lock);
    struct heap_block_header* block = (struct heap_block_header*)((unsigned char*)ptr - HEAP_HEADER_SIZE);

    if (block->is_free)
    {
        PANIC("HEAP: Double free detected.");
    }

    block->is_free = 1;
    heap_used_size -= block->size + HEAP_HEADER_SIZE;

    /* Attempt to coalesce physically adjacent free blocks */
    struct heap_block_header* curr = heap_free_list;
    while (curr)
    {
        if (curr->is_free && curr->next && curr->next->is_free)
        {
            unsigned char* curr_end = (unsigned char*)curr + HEAP_HEADER_SIZE + curr->size;
            if (curr_end == (unsigned char*)curr->next)
            {
                curr->size += HEAP_HEADER_SIZE + curr->next->size;
                curr->next = curr->next->next;
                continue;
            }
        }
        curr = curr->next;
    }
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
