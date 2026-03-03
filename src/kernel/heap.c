#include "heap.h"
#include "pmm.h"
#include "../lib/stdio.h"
#include "timer.h"

// each allocation is preceded by a block header
struct block_header
{
    unsigned long size;        // usable size (excluding header)
    struct block_header* next; // next block in free list (only valid when free)
    unsigned char free;        // 1 = free, 0 = allocated
};

#define HEADER_SIZE sizeof(struct block_header)
#define ALIGN(x) (((x) + 15) & ~15UL) // 16-byte alignment

static struct block_header* free_list;

// request a new page from PMM and turn it into a free block
static struct block_header* expand_heap(void)
{
    void* page = pmm_alloc_page();
    if (!page)
        return 0;

    struct block_header* block = (struct block_header*)page;
    block->size = PAGE_SIZE - HEADER_SIZE;
    block->next = 0;
    block->free = 1;

    return block;
}

void heap_init(void)
{
    free_list = expand_heap();
    if (!free_list)
    {
        printf("HEAP: Failed to initialize.\n");
        return;
    }

    printf("HEAP: Initialized with %d bytes.\n", (int)free_list->size);
}

void* kmalloc(unsigned long size)
{
    unsigned long int flags = irq_save();
    if (size == 0)
    {
        irq_restore(flags);
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
            irq_restore(flags);
            return (void*)((unsigned char*)curr + HEADER_SIZE);
        }

        prev = curr;
        curr = curr->next;
    }

    // no block found, get a new page
    struct block_header* new_page = expand_heap();
    if (!new_page)
    {
        printf("HEAP: Out of memory.\n");
        irq_restore(flags);
        return 0;
    }

    // append to end of free list
    if (prev)
        prev->next = new_page;
    else
        free_list = new_page;

    irq_restore(flags);
    // recurse to allocate from the new page
    return kmalloc(size);
}

void kfree(void* ptr)
{
    unsigned long flags = irq_save();
    if (!ptr)
    {
        irq_restore(flags);
        return;
    }
    struct block_header* block = (struct block_header*)((unsigned char*)ptr - HEADER_SIZE);

    if (block->free)
    {
        printf("HEAP: WARNING - Double free detected.\n");
        irq_restore(flags);
        return;
    }

    block->free = 1;

    // coalesce adjacent free blocks
    struct block_header* curr = free_list;
    while (curr)
    {
        if (curr->free && curr->next && curr->next->free)
        {
            curr->size += HEADER_SIZE + curr->next->size;
            curr->next = curr->next->next;
            continue; // check again
        }
        curr = curr->next;
    }
    irq_restore(flags);
}
