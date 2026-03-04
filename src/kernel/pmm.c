#include "pmm.h"
#include "timer.h"
#include "lock.h"
#include "../lib/stdio.h"
#include "../lib/panic.h"

extern char __kernel_end[];

#define KERNEL_VMA 0xFFFFFF8000000000ULL
#define V2P(v) ((unsigned long)(v) - KERNEL_VMA)
#define P2V(p) ((unsigned long)(p) + KERNEL_VMA)

#define PHYSICAL_MEMORY_SIZE (1024 * 1024 * 1024) // 1 GB
#define NUM_PAGES (PHYSICAL_MEMORY_SIZE / PAGE_SIZE)
#define MAX_ORDER 10 // up to 1024 pages (4MB blocks)

struct page
{
    struct page* next;
    unsigned int order;
    unsigned int is_free;
};

static struct page* page_array;
static struct page* free_lists[MAX_ORDER + 1];
static spinlock_t pmm_lock = SPINLOCK_INIT;

static inline unsigned int get_order(unsigned long count)
{
    unsigned int order = 0;
    unsigned long size = 1;
    while (size < count)
    {
        size <<= 1;
        order++;
    }
    return order;
}

static inline struct page* pfn_to_page(unsigned long pfn)
{
    return &page_array[pfn];
}
static inline unsigned long page_to_pfn(struct page* p)
{
    return p - page_array;
}

// merge neighbours (buddies :D) together recursevly
static void __free_buddy(unsigned long pfn, unsigned int order)
{
    while (order < MAX_ORDER)
    {
        // find sibling
        unsigned long buddy_pfn = pfn ^ (1UL << order);
        if (buddy_pfn >= NUM_PAGES)
            break;

        struct page* buddy = pfn_to_page(buddy_pfn);
        if (!buddy->is_free || buddy->order != order)
            break;

        // buddy is free.
        struct page** curr = &free_lists[order];
        while (*curr && *curr != buddy)
        {
            curr = &(*curr)->next;
        }
        if (*curr)
            *curr = buddy->next;

        buddy->is_free = 0;

        // merge the 2 blocks by taking the lowest page frame number
        pfn = (pfn < buddy_pfn) ? pfn : buddy_pfn;
        order++;
    }

    struct page* p = pfn_to_page(pfn);
    p->is_free = 1;
    p->order = order;
    p->next = free_lists[order];
    free_lists[order] = p;
}

void pmm_init(void)
{
    // align kernel end
    unsigned long kernel_end_aligned = ((unsigned long)__kernel_end + 7) & ~7UL;
    page_array = (struct page*)kernel_end_aligned;

    unsigned long array_size = NUM_PAGES * sizeof(struct page);
    unsigned long usable_start = (kernel_end_aligned + array_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    unsigned long reserved_pages = V2P(usable_start) / PAGE_SIZE;

    for (int i = 0; i <= MAX_ORDER; i++)
        free_lists[i] = 0;

    // mark everything as used
    for (unsigned long i = 0; i < NUM_PAGES; i++)
    {
        page_array[i].next = 0;
        page_array[i].order = 0;
        page_array[i].is_free = 0;
    }

    printf("[  PMM ] Buddy allocator: %lu pages total, %lu reserved (kernel+metadata)\n", (unsigned long)NUM_PAGES,
           reserved_pages);
    printf("[  PMM ] Page array: %lu KB metadata at 0x%lx\n", array_size / 1024, (unsigned long)page_array);
    printf("[  PMM ] Usable range: PFN %lu..%lu, max order %d (%lu KB blocks)\n", reserved_pages,
           (unsigned long)NUM_PAGES - 1, MAX_ORDER, (unsigned long)((1UL << MAX_ORDER) * PAGE_SIZE) / 1024);

    for (unsigned long pfn = reserved_pages; pfn < NUM_PAGES; pfn++)
    {
        __free_buddy(pfn, 0);
    }

    unsigned long free_pages = NUM_PAGES - reserved_pages;
    printf("[  PMM ] %lu MB free (%lu pages) — buddy system ready\n", (free_pages * PAGE_SIZE) / (1024 * 1024),
           free_pages);
}

void* pmm_alloc_pages(unsigned long count)
{
    if (count == 0)
        return 0;

    unsigned int target_order = get_order(count);
    if (target_order > MAX_ORDER)
        PANIC("PMM: Request too large!");

    unsigned long flags = spin_lock_irqsave(&pmm_lock);

    // find smallest block >= target_order
    int current_order = target_order;
    while (current_order <= MAX_ORDER && !free_lists[current_order])
    {
        current_order++;
    }

    if (current_order > MAX_ORDER)
    {
        spin_unlock_irqrestore(&pmm_lock, flags);
        PANIC("PMM: Out of Memory!");
    }

    // pop block off the list
    struct page* p = free_lists[current_order];
    free_lists[current_order] = p->next;
    p->is_free = 0;
    unsigned long pfn = page_to_pfn(p);

    // split the block in half until it matches the order
    while (current_order > target_order)
    {
        current_order--;
        unsigned long buddy_pfn = pfn + (1UL << current_order);
        struct page* buddy = pfn_to_page(buddy_pfn);

        buddy->is_free = 1;
        buddy->order = current_order;
        buddy->next = free_lists[current_order];
        free_lists[current_order] = buddy;
    }

    p->order = target_order;
    spin_unlock_irqrestore(&pmm_lock, flags);

    return (void*)(P2V(pfn * PAGE_SIZE));
}

void pmm_free_pages(void* ptr, unsigned long count)
{
    if (!ptr)
        return;

    unsigned long pfn = V2P(ptr) / PAGE_SIZE;
    unsigned int order = get_order(count);

    unsigned long flags = spin_lock_irqsave(&pmm_lock);
    __free_buddy(pfn, order);
    spin_unlock_irqrestore(&pmm_lock, flags);
}

void* pmm_alloc_page(void)
{
    return pmm_alloc_pages(1);
}
void pmm_free_page(void* ptr)
{
    pmm_free_pages(ptr, 1);
}
