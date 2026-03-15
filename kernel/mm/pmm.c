/*
 * pmm.c - Implementation of the Physical Memory Manager (PMM).
 *
 * This file implements a binary buddy allocator for physical memory management.
 * It handles discovery of physical RAM, reservation of critical ranges
 * (like the kernel image and device memory), and dynamic allocation of
 * contiguous page blocks.
 */

#include "pmm.h"

#include "types.h"
#include "lock.h"
#include "timer.h"
#include "stdio.h"
#include "panic.h"
#include "addr.h"

#include "devicetree/pht.h"

/* The end of the PMM metadata area, used by the MMU during initialization. */
unsigned long pmm_metadata_end = 0;

/* Internal constants for the buddy allocator */
#define PMM_MAX_ORDER           10
#define PMM_MAX_RESERVED_RANGES 16

/*
 * pmm_page - Structure representing a single physical memory page.
 * These structures are stored in a contiguous array at the start of managed RAM.
 */
struct pmm_page
{
    struct pmm_page* next; /* Next page in the free list or buddy chain */
    unsigned int order;    /* Buddy order (2^order pages) */
    unsigned int is_free;  /* Flag indicating if the page is currently unallocated */
    uint16_t refcount;     /* Number of active references to this page */
};

/*
 * pmm_reserved_range - Represents a range of physical memory excluded from the allocator.
 */
struct pmm_reserved_range
{
    unsigned long start_phys;
    unsigned long end_phys; /* Exclusive end address */
    const char* tag;        /* Human-readable name for the reserved range */
};

/* Extern kernel boundaries from the linker script */
extern char __kernel_end[];

/* Static state for the physical memory manager */
static unsigned long pmm_phys_mem_size = 1024 * 1024 * 1024; /* Default 1 GB */
static unsigned long pmm_num_pages;
static unsigned long pmm_managed_pages;
static unsigned long pmm_reserved_pages_count;
static unsigned long pmm_free_pages_count = 0;

static struct pmm_page* pmm_page_array;
static struct pmm_page* pmm_free_lists[PMM_MAX_ORDER + 1];

static struct pmm_reserved_range pmm_reserved_ranges[PMM_MAX_RESERVED_RANGES];
static unsigned int pmm_reserved_range_count = 0;

static spinlock_t pmm_lock = SPINLOCK_INIT;
static int pmm_ready       = 0;

/*
 * get_order - Calculates the smallest buddy order required to hold 'count' pages.
 */
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

/*
 * pfn_to_page - Converts a Physical Frame Number (PFN) to its corresponding page structure.
 */
static inline struct pmm_page* pfn_to_page(unsigned long pfn)
{
    return &pmm_page_array[pfn];
}

/*
 * page_to_pfn - Converts a page structure pointer back to its Physical Frame Number.
 */
static inline unsigned long page_to_pfn(struct pmm_page* p)
{
    return p - pmm_page_array;
}

/*
 * reserved_range_overlaps - Checks if two physical address ranges overlap.
 */
static int
reserved_range_overlaps(unsigned long a_start, unsigned long a_end, unsigned long b_start, unsigned long b_end)
{
    return a_start < b_end && b_start < a_end;
}

/*
 * pmm_reserve_range - Public API to reserve physical memory before initialization.
 */
void pmm_reserve_range(unsigned long phys_start, unsigned long size, const char* tag)
{
    if (size == 0)
    {
        return;
    }

    if (pmm_ready)
    {
        PANIC("PMM: pmm_reserve_range called after initialization");
    }

    unsigned long start = phys_start;
    unsigned long end   = phys_start + size;
    if (end < start)
    {
        PANIC("PMM: Reserved range address overflow");
    }

    /* Merge with existing reservations if they overlap or are adjacent */
    for (unsigned int i = 0; i < pmm_reserved_range_count; i++)
    {
        unsigned long cur_start = pmm_reserved_ranges[i].start_phys;
        unsigned long cur_end   = pmm_reserved_ranges[i].end_phys;
        if (reserved_range_overlaps(start, end, cur_start, cur_end) || end == cur_start || start == cur_end)
        {
            if (start < pmm_reserved_ranges[i].start_phys)
            {
                pmm_reserved_ranges[i].start_phys = start;
            }
            if (end > pmm_reserved_ranges[i].end_phys)
            {
                pmm_reserved_ranges[i].end_phys = end;
            }
            return;
        }
    }

    if (pmm_reserved_range_count >= PMM_MAX_RESERVED_RANGES)
    {
        PANIC("PMM: Exceeded maximum number of reserved ranges");
    }

    pmm_reserved_ranges[pmm_reserved_range_count].start_phys = start;
    pmm_reserved_ranges[pmm_reserved_range_count].end_phys   = end;
    pmm_reserved_ranges[pmm_reserved_range_count].tag        = tag;
    pmm_reserved_range_count++;
}

/*
 * range_overlaps_reserved_pfns - Checks if a PFN range intersects any reserved ranges.
 */
static inline int range_overlaps_reserved_pfns(unsigned long start_pfn, unsigned long end_pfn)
{
    for (unsigned int i = 0; i < pmm_reserved_range_count; i++)
    {
        unsigned long r_start = pmm_reserved_ranges[i].start_phys / PAGE_SIZE;
        unsigned long r_end   = (pmm_reserved_ranges[i].end_phys + PAGE_SIZE - 1) / PAGE_SIZE;

        if (r_start >= pmm_num_pages)
        {
            continue;
        }
        if (r_end > pmm_num_pages)
        {
            r_end = pmm_num_pages;
        }
        if (r_start < r_end && start_pfn < r_end && r_start < end_pfn)
        {
            return 1;
        }
    }
    return 0;
}

/*
 * pfn_is_reserved - Checks if a single Physical Frame Number is in a reserved range.
 */
static inline int pfn_is_reserved(unsigned long pfn)
{
    return range_overlaps_reserved_pfns(pfn, pfn + 1);
}

/*
 * count_reserved_pages - Calculates the total number of pages currently marked as reserved.
 */
static unsigned long count_reserved_pages(void)
{
    unsigned long count = 0;
    for (unsigned long pfn = 0; pfn < pmm_num_pages; pfn++)
    {
        if (pfn_is_reserved(pfn))
        {
            count++;
        }
    }
    return count;
}

/*
 * pmm_free_buddy_internal - Reinserts a block into the buddy system,
 * merging with its buddy if possible.
 */
static void pmm_free_buddy_internal(unsigned long pfn, unsigned int order)
{
    pmm_free_pages_count += (1UL << order);
    while (order < PMM_MAX_ORDER)
    {
        unsigned long buddy_pfn = pfn ^ (1UL << order);
        if (buddy_pfn >= pmm_num_pages)
        {
            break;
        }

        struct pmm_page* buddy = pfn_to_page(buddy_pfn);
        if (!buddy->is_free || buddy->order != order)
        {
            break;
        }

        /* Remove buddy from its current free list to perform the merge */
        struct pmm_page** curr = &pmm_free_lists[order];
        while (*curr && *curr != buddy)
        {
            curr = &(*curr)->next;
        }
        if (*curr)
        {
            *curr = buddy->next;
        }

        buddy->is_free = 0;
        pfn            = (pfn < buddy_pfn) ? pfn : buddy_pfn;
        order++;
    }

    struct pmm_page* p    = pfn_to_page(pfn);
    p->is_free            = 1;
    p->order              = order;
    p->next               = pmm_free_lists[order];
    pmm_free_lists[order] = p;
}

/*
 * pmm_init - Initializes the physical memory structures and free lists.
 */
void pmm_init(void)
{
    struct pht_node* mem_node = pht_find_device("memory");
    if (mem_node)
    {
        pmm_phys_mem_size = mem_node->size[0];
    }
    pmm_num_pages = pmm_phys_mem_size / PAGE_SIZE;

    /* The page structure array follows the kernel image in memory */
    unsigned long kernel_end_aligned = ((unsigned long)__kernel_end + 7) & ~7UL;
    pmm_page_array                   = (struct pmm_page*)kernel_end_aligned;

    unsigned long array_size   = pmm_num_pages * sizeof(struct pmm_page);
    unsigned long usable_start = (kernel_end_aligned + array_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    pmm_metadata_end           = usable_start;
    pmm_reserved_pages_count   = V2P(usable_start) / PAGE_SIZE;

    /* Ensure kernel code and PMM metadata are never allocated */
    pmm_reserve_range(0, V2P(usable_start), "kernel+metadata");

    for (int i = 0; i <= PMM_MAX_ORDER; i++)
    {
        pmm_free_lists[i] = (void*)0;
    }

    for (unsigned long i = 0; i < pmm_num_pages; i++)
    {
        pmm_page_array[i].next     = (void*)0;
        pmm_page_array[i].order    = 0;
        pmm_page_array[i].is_free  = 0;
        pmm_page_array[i].refcount = 0;
    }

    unsigned long reserved_total = count_reserved_pages();
    printf("[  PMM ] Buddy system: %lu pages total, %lu reserved\n", pmm_num_pages, reserved_total);
    printf("[  PMM ] Metadata: %lu KB at 0x%lx\n", array_size / 1024, (unsigned long)pmm_page_array);

    for (unsigned int i = 0; i < pmm_reserved_range_count; i++)
    {
        unsigned long start = pmm_reserved_ranges[i].start_phys;
        unsigned long end   = pmm_reserved_ranges[i].end_phys;
        if (start >= end)
        {
            continue;
        }
        const char* tag = pmm_reserved_ranges[i].tag ? pmm_reserved_ranges[i].tag : "(untagged)";
        printf("[  PMM ] reserve: 0x%lx..0x%lx (%s)\n", start, end - 1, tag);
    }

    /* Add all unreserved physical memory to the buddy free lists */
    unsigned long pfn = 0;
    while (pfn < pmm_num_pages)
    {
        if (pfn_is_reserved(pfn))
        {
            pfn++;
            continue;
        }

        unsigned int order = PMM_MAX_ORDER;
        while (order > 0
               && ((pfn & ((1UL << order) - 1)) != 0 || pfn + (1UL << order) > pmm_num_pages
                   || range_overlaps_reserved_pfns(pfn, pfn + (1UL << order))))
        {
            order--;
        }

        pmm_free_buddy_internal(pfn, order);
        pfn += (1UL << order);
    }

    pmm_managed_pages = pmm_free_pages_count;
    pmm_ready         = 1;

    printf("[  PMM ] %lu MB free — buddy system ready\n", (pmm_free_pages_count * PAGE_SIZE) / (1024 * 1024));
}

/*
 * pmm_alloc_pages - Core implementation of contiguous page allocation.
 */
void* pmm_alloc_pages(unsigned long count)
{
    if (count == 0)
    {
        return (void*)0;
    }

    unsigned int target_order = get_order(count);
    if (target_order > PMM_MAX_ORDER)
    {
        PANIC("PMM: Allocation request exceeds maximum buddy order");
    }

    unsigned long flags = spin_lock_irqsave(&pmm_lock);

    /* Find the smallest available block that can satisfy the request */
    unsigned int current_order = target_order;
    while (current_order <= PMM_MAX_ORDER && !pmm_free_lists[current_order])
    {
        current_order++;
    }

    if (current_order > PMM_MAX_ORDER)
    {
        spin_unlock_irqrestore(&pmm_lock, flags);
        PANIC("PMM: Physical memory exhaustion");
        return (void*)0;
    }

    struct pmm_page* p            = pmm_free_lists[current_order];
    pmm_free_lists[current_order] = p->next;
    p->is_free                    = 0;
    pmm_free_pages_count -= (1UL << target_order);
    unsigned long pfn = page_to_pfn(p);

    /* Split larger blocks into buddies until we reach the target order */
    while (current_order > target_order)
    {
        current_order--;
        unsigned long buddy_pfn = pfn + (1UL << current_order);
        struct pmm_page* buddy  = pfn_to_page(buddy_pfn);

        buddy->is_free                = 1;
        buddy->order                  = current_order;
        buddy->next                   = pmm_free_lists[current_order];
        pmm_free_lists[current_order] = buddy;
    }

    p->order    = target_order;
    p->refcount = 1;
    spin_unlock_irqrestore(&pmm_lock, flags);

    return (void*)(P2V(pfn * PAGE_SIZE));
}

/*
 * pmm_free_pages - Core implementation of page deallocation.
 */
void pmm_free_pages(void* ptr, unsigned long count)
{
    if (!ptr)
    {
        return;
    }

    unsigned long pfn  = V2P(ptr) / PAGE_SIZE;
    unsigned int order = get_order(count);

    if (pfn >= pmm_num_pages || pfn_is_reserved(pfn))
    {
        return;
    }

    unsigned long flags = spin_lock_irqsave(&pmm_lock);
    struct pmm_page* p  = &pmm_page_array[pfn];

    if (p->is_free || p->refcount == 0)
    {
        PANIC("PMM: Attempted to free an unallocated page");
    }

    p->refcount--;
    if (p->refcount == 0)
    {
        pmm_free_buddy_internal(pfn, order);
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
}

/*
 * pmm_hold_page - Increments the reference count of a physical page.
 */
void pmm_hold_page(void* ptr)
{
    if (!ptr)
    {
        return;
    }
    unsigned long pfn = V2P(ptr) / PAGE_SIZE;

    if (pfn >= pmm_num_pages || pfn_is_reserved(pfn))
    {
        return;
    }

    unsigned long flags = spin_lock_irqsave(&pmm_lock);
    pmm_page_array[pfn].refcount++;
    spin_unlock_irqrestore(&pmm_lock, flags);
}

/*
 * pmm_alloc_page - Allocates a single page.
 */
void* pmm_alloc_page(void)
{
    return pmm_alloc_pages(1);
}

/*
 * pmm_free_page - Frees a single page.
 */
void pmm_free_page(void* ptr)
{
    pmm_free_pages(ptr, 1);
}

/*
 * pmm_get_free_pages - Returns the count of free pages.
 */
unsigned long pmm_get_free_pages(void)
{
    return pmm_free_pages_count;
}

/*
 * pmm_get_total_pages - Returns the count of managed pages.
 */
unsigned long pmm_get_total_pages(void)
{
    return pmm_managed_pages;
}
