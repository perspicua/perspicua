#include "pmm.h"
#include "timer.h"
#include "lock.h"
#include "stdio.h"
#include "panic.h"
#include "addr.h"
#include "devicetree/pht.h"

extern char __kernel_end[];
unsigned long pmm_metadata_end = 0;

static unsigned long physical_memory_size = 1024 * 1024 * 1024; // Default 1 GB
static unsigned long num_pages;
static unsigned long managed_pages_count;
#define MAX_ORDER 10 // up to 1024 pages (4MB blocks)
#define MAX_RESERVED_RANGES 16

struct page
{
    struct page* next;
    unsigned int order;
    unsigned int is_free;
    uint16_t refcount;
};

static struct page* page_array;
static struct page* free_lists[MAX_ORDER + 1];
static unsigned long pmm_reserved_pages;
static unsigned long free_pages_count = 0;
static spinlock_t pmm_lock = SPINLOCK_INIT;
static int pmm_ready = 0;

struct reserved_range
{
    unsigned long start_phys;
    unsigned long end_phys; // exclusive
    const char* tag;
};

static struct reserved_range reserved_ranges[MAX_RESERVED_RANGES];
static unsigned int reserved_range_count = 0;

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

static int reserved_range_overlaps(unsigned long a_start, unsigned long a_end, unsigned long b_start, unsigned long b_end)
{
    return a_start < b_end && b_start < a_end;
}

void pmm_reserve_range(unsigned long phys_start, unsigned long size, const char* tag)
{
    if (size == 0)
        return;

    if (pmm_ready)
        PANIC("PMM: pmm_reserve_range must be called before pmm_init()\n");

    unsigned long start = phys_start;
    unsigned long end = phys_start + size;
    if (end < start)
        PANIC("PMM: reserve range overflow\n");

    // Merge overlapping/adjacent reservations to keep the table compact.
    for (unsigned int i = 0; i < reserved_range_count; i++)
    {
        unsigned long cur_start = reserved_ranges[i].start_phys;
        unsigned long cur_end = reserved_ranges[i].end_phys;
        if (reserved_range_overlaps(start, end, cur_start, cur_end) || end == cur_start || start == cur_end)
        {
            if (start < reserved_ranges[i].start_phys)
                reserved_ranges[i].start_phys = start;
            if (end > reserved_ranges[i].end_phys)
                reserved_ranges[i].end_phys = end;
            return;
        }
    }

    if (reserved_range_count >= MAX_RESERVED_RANGES)
        PANIC("PMM: too many reserved ranges\n");

    reserved_ranges[reserved_range_count].start_phys = start;
    reserved_ranges[reserved_range_count].end_phys = end;
    reserved_ranges[reserved_range_count].tag = tag;
    reserved_range_count++;
}

static inline int range_overlaps_reserved_pfns(unsigned long start_pfn, unsigned long end_pfn)
{
    for (unsigned int i = 0; i < reserved_range_count; i++)
    {
        unsigned long r_start = reserved_ranges[i].start_phys / PAGE_SIZE;
        unsigned long r_end = (reserved_ranges[i].end_phys + PAGE_SIZE - 1) / PAGE_SIZE;

        if (r_start >= num_pages)
            continue;
        if (r_end > num_pages)
            r_end = num_pages;
        if (r_start < r_end && start_pfn < r_end && r_start < end_pfn)
            return 1;
    }
    return 0;
}

static inline int pfn_is_reserved(unsigned long pfn)
{
    return range_overlaps_reserved_pfns(pfn, pfn + 1);
}

static unsigned long count_reserved_pages(void)
{
    unsigned long count = 0;
    for (unsigned long pfn = 0; pfn < num_pages; pfn++)
    {
        if (pfn_is_reserved(pfn))
            count++;
    }
    return count;
}

static void __free_buddy(unsigned long pfn, unsigned int order)
{
    free_pages_count += (1UL << order);
    while (order < MAX_ORDER)
    {
        unsigned long buddy_pfn = pfn ^ (1UL << order);
        if (buddy_pfn >= num_pages)
            break;

        struct page* buddy = pfn_to_page(buddy_pfn);
        if (!buddy->is_free || buddy->order != order)
            break;

        struct page** curr = &free_lists[order];
        while (*curr && *curr != buddy)
        {
            curr = &(*curr)->next;
        }
        if (*curr)
            *curr = buddy->next;

        buddy->is_free = 0;
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
    struct pht_node* mem_node = pht_find_device("memory");
    if (mem_node)
    {
        physical_memory_size = mem_node->size[0];
    }
    num_pages = physical_memory_size / PAGE_SIZE;

    unsigned long kernel_end_aligned = ((unsigned long)__kernel_end + 7) & ~7UL;
    page_array = (struct page*)kernel_end_aligned;

    unsigned long array_size = num_pages * sizeof(struct page);
    unsigned long usable_start = (kernel_end_aligned + array_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    pmm_metadata_end = usable_start;
    unsigned long reserved_pages = V2P(usable_start) / PAGE_SIZE;
    pmm_reserved_pages = reserved_pages;

    // Keep kernel image + PMM metadata out of buddy free lists.
    pmm_reserve_range(0, V2P(usable_start), "kernel+metadata");

    for (int i = 0; i <= MAX_ORDER; i++)
        free_lists[i] = 0;

    for (unsigned long i = 0; i < num_pages; i++)
    {
        page_array[i].next = 0;
        page_array[i].order = 0;
        page_array[i].is_free = 0;
        page_array[i].refcount = 0;
    }

        unsigned long reserved_total_pages = count_reserved_pages();
        printf("[  PMM ] Buddy allocator: %lu pages total, %lu reserved\n", (unsigned long)num_pages,
               reserved_total_pages);
    printf("[  PMM ] Page array: %lu KB metadata at 0x%lx\n", array_size / 1024, (unsigned long)page_array);
        printf("[  PMM ] Usable range: PFN %lu..%lu, max order %d (%lu KB blocks)\n", reserved_pages,
           (unsigned long)num_pages - 1, MAX_ORDER, (unsigned long)((1UL << MAX_ORDER) * PAGE_SIZE) / 1024);

        for (unsigned int i = 0; i < reserved_range_count; i++)
        {
            unsigned long start = reserved_ranges[i].start_phys;
            unsigned long end = reserved_ranges[i].end_phys;
            if (start >= end)
                continue;
            const char* tag = reserved_ranges[i].tag ? reserved_ranges[i].tag : "(untagged)";
            printf("[  PMM ] reserve: 0x%lx..0x%lx (%s)\n", start, end - 1, tag);
        }

        unsigned long pfn = 0;
    while (pfn < num_pages)
    {
            if (pfn_is_reserved(pfn))
        {
                pfn++;
            continue;
        }

        unsigned int order = MAX_ORDER;
            while (order > 0 && ((pfn & ((1UL << order) - 1)) != 0 || pfn + (1UL << order) > num_pages ||
                                 range_overlaps_reserved_pfns(pfn, pfn + (1UL << order))))
        {
            order--;
        }

        __free_buddy(pfn, order);
        pfn += (1UL << order);
    }

        managed_pages_count = free_pages_count;
        pmm_ready = 1;

    printf("[  PMM ] %lu MB free (%lu pages) — buddy system ready\n",
           (free_pages_count * PAGE_SIZE) / (1024 * 1024), free_pages_count);
}

void* pmm_alloc_pages(unsigned long count)
{
    if (count == 0)
        return 0;

    unsigned int target_order = get_order(count);
    if (target_order > MAX_ORDER)
        PANIC("PMM: Request too large!");

    unsigned long flags = spin_lock_irqsave(&pmm_lock);

    unsigned int current_order = target_order;
    while (current_order <= MAX_ORDER && !free_lists[current_order])
    {
        current_order++;
    }

    if (current_order > MAX_ORDER)
    {
        spin_unlock_irqrestore(&pmm_lock, flags);
        PANIC("PMM: Out of Memory!");
        return 0;
    }

    struct page* p = free_lists[current_order];
    free_lists[current_order] = p->next;
    p->is_free = 0;
    free_pages_count -= (1UL << target_order);
    unsigned long pfn = page_to_pfn(p);

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
    p->refcount = 1;
    spin_unlock_irqrestore(&pmm_lock, flags);

    return (void*)(P2V(pfn * PAGE_SIZE));
}

void pmm_free_pages(void* ptr, unsigned long count)
{
    if (!ptr)
        return;

    unsigned long pfn = V2P(ptr) / PAGE_SIZE;
    unsigned int order = get_order(count);

    if (pfn >= num_pages || pfn_is_reserved(pfn))
        return;

    ASSERT(pfn + (1UL << order) <= num_pages);

    unsigned long flags = spin_lock_irqsave(&pmm_lock);
    struct page* p = &page_array[pfn];
    ASSERT(!p->is_free);
    ASSERT(p->refcount > 0);

    p->refcount--;
    if (p->refcount == 0)
    {
        __free_buddy(pfn, order);
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
}

void pmm_hold_page(void* ptr)
{
    if (!ptr)
        return;
    unsigned long pfn = V2P(ptr) / PAGE_SIZE;

    if (pfn >= num_pages || pfn_is_reserved(pfn))
        return;

    unsigned long flags = spin_lock_irqsave(&pmm_lock);
    page_array[pfn].refcount++;
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

unsigned long pmm_get_free_pages(void)
{
    return free_pages_count;
}

unsigned long pmm_get_total_pages(void)
{
    return managed_pages_count;
}
