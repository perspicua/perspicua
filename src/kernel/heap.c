#include "heap.h"
#include "lock.h"
#include "pmm.h"
#include "../lib/stdio.h"
#include "../lib/panic.h"
#include "timer.h"

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

    return block;
}

void heap_init(void)
{
    free_list = expand_heap(PAGE_SIZE);
    if (!free_list)
    {
        PANIC("HEAP: Failed to initialize.\n");
        return;
    }

    printf("HEAP: Initialized with %d bytes.\n", (int)free_list->size);
}

void* kmalloc(unsigned long size)
{
    unsigned long int flags = spin_lock_irqsave(&heap_lock);
    if (size == 0)
    {
        spin_unlock_irqrestore(&heap_lock, flags);
        return 0;
    }

    size = ALIGN(size);

    // first-fit search
    struct block_header* curr = free_list;
    struct block_header* prev = 0;

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
            spin_unlock_irqrestore(&heap_lock, flags);
            return (void*)((unsigned char*)curr + HEADER_SIZE);
        }

        prev = curr;
        curr = curr->next;
    }

    // no block found, expand heap with enough space for this allocation
    struct block_header* new_page = expand_heap(size);
    if (!new_page)
    {
        PANIC("HEAP: Out of memory.\n");
        spin_unlock_irqrestore(&heap_lock, flags);
        return 0;
    }

    // append to end of free list
    if (prev)
        prev->next = new_page;
    else
        free_list = new_page;

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
    spin_unlock_irqrestore(&heap_lock, flags);
    return (void*)((unsigned char*)new_page + HEADER_SIZE);
}

void kfree(void* ptr)
{
    unsigned long flags = spin_lock_irqsave(&heap_lock);
    if (!ptr)
    {
        spin_unlock_irqrestore(&heap_lock, flags);
        return;
    }
    struct block_header* block = (struct block_header*)((unsigned char*)ptr - HEADER_SIZE);

    if (block->free)
    {
        PANIC("HEAP: WARNING - Double free detected.\n");
        spin_unlock_irqrestore(&heap_lock, flags);
        return;
    }

    block->free = 1;

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
