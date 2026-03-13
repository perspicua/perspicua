#include "heap.h"
#include "slab.h"
#include "lock.h"
#include "pmm.h"
#include "stdio.h"
#include "panic.h"
#include "timer.h"

#define SLAB_MAX 1024 // allocations <= this go through the slab layer

// each allocation is preceded by a block header
struct block_header
{
    unsigned long size;        // usable size (excluding header)
    struct block_header* next; // next block in free list (only valid when free)
    unsigned char free;        // 1 = free, 0 = allocated
} __attribute__((aligned(16)));

#define HEADER_SIZE sizeof(struct block_header)
#define ALIGN(x) (((x) + 15) & ~15UL) // 16-byte alignment

static struct block_header* free_list;
static spinlock_t heap_lock = SPINLOCK_INIT;
static unsigned long heap_total_size = 0;
static unsigned long heap_used_size = 0;

// request enough contiguous pages from PMM to satisfy at least 'min_size' bytes
static struct block_header* expand_heap(unsigned long min_size)
{
    unsigned long total = min_size + HEADER_SIZE;
    unsigned long pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;

    void* region = pmm_alloc_pages(pages);
    if (!region)
        return 0;

    struct block_header* block = (struct block_header*)region;
    block->size = pages * PAGE_SIZE - HEADER_SIZE;
    block->next = 0;
    block->free = 1;

    heap_total_size += pages * PAGE_SIZE;

    return block;
}

void heap_init(void)
{
    slab_init();

    free_list = expand_heap(PAGE_SIZE);
    if (!free_list)
    {
        PANIC("HEAP: Failed to initialize.\n");
        return;
    }

    printf("[ HEAP ] First-fit fallback: %lu bytes initial pool\n", free_list->size);
    printf("[ HEAP ] Header: %lu bytes, payload alignment: 16 bytes\n", (unsigned long)HEADER_SIZE);
}

void* kmalloc(unsigned long size)
{
    if (size == 0)
        return 0;
    // small allocations: O(1) slab path
    if (size <= SLAB_MAX)
        return slab_alloc(size);

    // large allocations: first-fit fallback
    unsigned long int flags = spin_lock_irqsave(&heap_lock);

    size = ALIGN(size);

    // first-fit search
    struct block_header* curr = free_list;

    while (curr)
    {
        if (curr->free && curr->size >= size)
        {
            // split if remaining space can hold another block
            unsigned long remaining = curr->size - size - HEADER_SIZE;
            if (curr->size >= size + HEADER_SIZE + 16)
            {
                struct block_header* new_block = (struct block_header*)((unsigned char*)curr + HEADER_SIZE + size);
                new_block->size = remaining;
                new_block->next = curr->next;
                new_block->free = 1;

                curr->size = size;
                curr->next = new_block;
            }

            curr->free = 0;
            heap_used_size += curr->size + HEADER_SIZE;
            spin_unlock_irqrestore(&heap_lock, flags);
            return (void*)((unsigned char*)curr + HEADER_SIZE);
        }

        curr = curr->next;
    }

    // no block found, expand heap with enough space for this allocation
    struct block_header* new_page = expand_heap(size);
    if (!new_page)
        PANIC("HEAP: Out of memory.\n");
    // insert in address order so coalescing works across regions
    if (!free_list || (unsigned long)new_page < (unsigned long)free_list)
    {
        new_page->next = free_list;
        free_list = new_page;
    }
    else
    {
        struct block_header* scan = free_list;
        while (scan->next && (unsigned long)scan->next < (unsigned long)new_page)
            scan = scan->next;
        new_page->next = scan->next;
        scan->next = new_page;
    }

    // allocate directly from the new block while still holding the lock
    // (avoids race where another CPU steals the block between unlock and re-lock)
    if (new_page->size >= size + HEADER_SIZE + 16)
    {
        struct block_header* split = (struct block_header*)((unsigned char*)new_page + HEADER_SIZE + size);
        split->size = new_page->size - size - HEADER_SIZE;
        split->next = new_page->next;
        split->free = 1;

        new_page->size = size;
        new_page->next = split;
    }

    new_page->free = 0;
    heap_used_size += new_page->size + HEADER_SIZE;
    spin_unlock_irqrestore(&heap_lock, flags);
    return (void*)((unsigned char*)new_page + HEADER_SIZE);
}

void kfree(void* ptr)
{
    if (!ptr)
        return;

    // if the slab allocator owns this pointer, free through slab
    if (slab_owns(ptr))
    {
        slab_free(ptr);
        return;
    }

    // large-allocation free: first-fit path
    unsigned long flags = spin_lock_irqsave(&heap_lock);
    struct block_header* block = (struct block_header*)((unsigned char*)ptr - HEADER_SIZE);

    if (block->free)
        PANIC("HEAP: Double free detected.\n");

    block->free = 1;
    heap_used_size -= block->size + HEADER_SIZE;

    // coalesce physically adjacent free blocks
    struct block_header* curr = free_list;
    while (curr)
    {
        if (curr->free && curr->next && curr->next->free)
        {
            // only merge if blocks are physically contiguous in memory
            unsigned char* curr_end = (unsigned char*)curr + HEADER_SIZE + curr->size;
            if (curr_end == (unsigned char*)curr->next)
            {
                curr->size += HEADER_SIZE + curr->next->size;
                curr->next = curr->next->next;
                continue; // check again
            }
        }
        curr = curr->next;
    }
    spin_unlock_irqrestore(&heap_lock, flags);
}

unsigned long heap_get_used(void)
{
    return heap_used_size + slab_get_used();
}

unsigned long heap_get_total(void)
{
    return heap_total_size + slab_get_total();
}
