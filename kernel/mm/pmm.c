/*
 * pmm.c - Binary Buddy Physical Memory Manager.
 *
 * Implements a buddy system for physical memory allocation, reference
 * counting, and zeroing pages on allocation and deallocation.
 */

#include "mm/pmm.h"
#include "mm/addr.h"
#include "types.h"
#include "core/lock.h"
#include "stdio.h"
#include "string.h"
#include "panic.h"
#include "devicetree/fdt.h"

unsigned long pmm_metadata_end = 0;

/* Internal constants and types */
#define PMM_MAX_FREE_LIST_SCAN 131072UL

struct pmm_page
{
    struct pmm_page* next; /* Next free block in the buddy list */
    uint16_t refcount;     /* Active reference count */
    uint8_t order;         /* Allocation order */
    uint8_t is_free;       /* 1 if on free list, 0 if allocated */
    uint8_t _pad[4];
};

_Static_assert(sizeof(struct pmm_page) == 16, "pmm_page must be 16 bytes");

struct pmm_reserved_range
{
    unsigned long start_phys;
    unsigned long end_phys;
    const char* tag;
};

extern char __kernel_end[];

static unsigned long pmm_phys_mem_size = 1024UL * 1024 * 1024;
static unsigned long pmm_num_pages = 0;
static unsigned long pmm_managed_pages = 0;
static unsigned long pmm_free_pages_count = 0;

static struct pmm_page* pmm_page_array = NULL;
static struct pmm_page* pmm_free_lists[PMM_MAX_ORDER + 1];
static struct pmm_reserved_range pmm_reserved_ranges[PMM_MAX_RESERVED_RANGES];
static unsigned int pmm_reserved_range_count = 0;

static spinlock_t pmm_lock = SPINLOCK_INIT;
static int pmm_ready = 0;

/* Small utilities */
static inline unsigned int get_order(unsigned long count)
{
    if (count == 0)
        return 0;

    unsigned int order = 0;
    unsigned long size = 1UL;

    while (size < count)
    {
        if (order >= PMM_MAX_ORDER)
            return PMM_MAX_ORDER + 1;
        size <<= 1;
        order++;
    }
    return order;
}

static inline struct pmm_page* pfn_to_page(unsigned long pfn)
{
    return &pmm_page_array[pfn];
}

static inline unsigned long page_to_pfn(const struct pmm_page* p)
{
    return (unsigned long)(p - pmm_page_array);
}

static int
reserved_range_overlaps(unsigned long a_start, unsigned long a_end, unsigned long b_start, unsigned long b_end)
{
    return a_start < b_end && b_start < a_end;
}

/* Check if PFN range overlaps any reserved range */
static int range_overlaps_reserved_pfns(unsigned long start_pfn, unsigned long end_pfn)
{
    for (unsigned int i = 0; i < pmm_reserved_range_count; i++)
    {
        unsigned long rstart = pmm_reserved_ranges[i].start_phys;
        unsigned long rend = pmm_reserved_ranges[i].end_phys;

        unsigned long r_pfn_start = rstart / PAGE_SIZE;
        unsigned long r_pfn_end;

        if (rend >= (unsigned long)(-1) - (PAGE_SIZE - 1))
            r_pfn_end = (unsigned long)(-1) / PAGE_SIZE + 1;
        else
            r_pfn_end = (rend + PAGE_SIZE - 1) / PAGE_SIZE;

        if (r_pfn_start >= pmm_num_pages)
            continue;
        if (r_pfn_end > pmm_num_pages)
            r_pfn_end = pmm_num_pages;

        if (r_pfn_start < r_pfn_end && start_pfn < r_pfn_end && r_pfn_start < end_pfn)
        {
            return 1;
        }
    }
    return 0;
}

static inline int pfn_is_reserved(unsigned long pfn)
{
    return range_overlaps_reserved_pfns(pfn, pfn + 1);
}

void pmm_reserve_range(unsigned long phys_start, unsigned long size, const char* tag)
{
    if (size == 0)
        return;

    if (pmm_ready)
        PANIC("PMM: reserve called after init");

    unsigned long start = phys_start;
    unsigned long end = phys_start + size;

    if (end < start)
        PANIC("PMM: reserve overflow");

    // Merge with existing ranges
    int merged;
    do
    {
        merged = 0;
        for (unsigned int i = 0; i < pmm_reserved_range_count; i++)
        {
            unsigned long cs = pmm_reserved_ranges[i].start_phys;
            unsigned long ce = pmm_reserved_ranges[i].end_phys;

            if (reserved_range_overlaps(start, end, cs, ce) || end == cs || start == ce)
            {
                if (cs < start)
                    start = cs;
                if (ce > end)
                    end = ce;

                unsigned int last = pmm_reserved_range_count - 1;
                pmm_reserved_ranges[i] = pmm_reserved_ranges[last];
                pmm_reserved_range_count--;
                merged = 1;
                break;
            }
        }
    } while (merged);

    if (pmm_reserved_range_count >= PMM_MAX_RESERVED_RANGES)
        PANIC("PMM: max reserved ranges exceeded");

    unsigned int slot = pmm_reserved_range_count++;
    pmm_reserved_ranges[slot].start_phys = start;
    pmm_reserved_ranges[slot].end_phys = end;
    pmm_reserved_ranges[slot].tag = tag;
}

/* Internal: insert block into buddy system and merge upwards */
static void pmm_free_buddy_internal(unsigned long pfn, unsigned int order)
{
    while (order < PMM_MAX_ORDER)
    {
        unsigned long buddy_pfn = pfn ^ (1UL << order);
        if (buddy_pfn >= pmm_num_pages)
            break;

        struct pmm_page* buddy = pfn_to_page(buddy_pfn);
        if (!buddy->is_free || buddy->order != order)
            break;

        // Remove buddy from free list
        struct pmm_page** curr = &pmm_free_lists[order];
        unsigned long scan = 0;
        while (*curr && *curr != buddy)
        {
            curr = &(*curr)->next;
            scan++;
            if (scan > PMM_MAX_FREE_LIST_SCAN)
            {
                PANIC("PMM: free-list corruption detected");
            }
        }

        if (!*curr)
        {
            PANIC("PMM: buddy is_free=1 but not on free list");
        }

        *curr = buddy->next;
        buddy->next = NULL;
        buddy->is_free = 0;
        buddy->order = 0;

        pfn = (pfn < buddy_pfn) ? pfn : buddy_pfn;
        order++;
    }

    struct pmm_page* p = pfn_to_page(pfn);
    p->is_free = 1;
    p->order = (uint8_t)order;
    p->next = pmm_free_lists[order];
    pmm_free_lists[order] = p;
}

void pmm_init(void)
{
    // Step 1: Discover memory from DTB
    const uint32_t* mem_node = fdt_find_node_by_path("/memory@0");
    if (mem_node)
    {
        struct fdt_property reg_prop;
        if (fdt_get_property(mem_node, "reg", &reg_prop) == 0)
        {
            const uint32_t* reg = (const uint32_t*)reg_prop.value;
            uint32_t mem_size_cells = fdt32_to_cpu(reg[2]);

            if (mem_size_cells == 0)
                PANIC("PMM: DTB reports zero memory");

            pmm_phys_mem_size = (unsigned long)mem_size_cells;
            printf("[  PMM ] Physical memory from DTB: %lu MB\n", pmm_phys_mem_size / (1024UL * 1024));
        }
    }
    else
    {
        PANIC("PMM: could not find memory node");
    }

    pmm_num_pages = pmm_phys_mem_size / PAGE_SIZE;
    if (pmm_num_pages == 0)
        PANIC("PMM: physical memory too small");

    // Step 2: Descriptor array allocation
    unsigned long kernel_end = (unsigned long)__kernel_end;
    unsigned long array_start = (kernel_end + 15UL) & ~15UL;
    unsigned long array_bytes = pmm_num_pages * sizeof(struct pmm_page);

    if (array_bytes / sizeof(struct pmm_page) != pmm_num_pages)
        PANIC("PMM: array size overflow");

    pmm_page_array = (struct pmm_page*)array_start;
    unsigned long usable_start_va = (array_start + array_bytes + PAGE_SIZE - 1) & ~(unsigned long)(PAGE_SIZE - 1);
    pmm_metadata_end = usable_start_va;

    // Step 3: Reserve kernel + metadata
    unsigned long usable_start_phys = V2P(usable_start_va);
    if (usable_start_phys >= pmm_phys_mem_size)
    {
        PANIC("PMM: metadata exceeds RAM");
    }
    pmm_reserve_range(0, usable_start_phys, "kernel+metadata");

    // Step 4: Initialise descriptors
    for (int i = 0; i <= PMM_MAX_ORDER; i++)
        pmm_free_lists[i] = NULL;

    memset(pmm_page_array, 0, array_bytes);

    printf("[  PMM ] %lu pages, descriptor array: %lu KB at 0x%lx\n", pmm_num_pages, array_bytes / 1024, array_start);

    for (unsigned int i = 0; i < pmm_reserved_range_count; i++)
    {
        unsigned long s = pmm_reserved_ranges[i].start_phys;
        unsigned long e = pmm_reserved_ranges[i].end_phys;
        if (s >= e)
            continue;
        printf("[  PMM ] reserve 0x%lx..0x%lx (%s)\n", s, e - 1, pmm_reserved_ranges[i].tag ?: "(untagged)");
    }

    // Step 5: Populate free lists
    unsigned long pfn = 0;
    while (pfn < pmm_num_pages)
    {
        if (pfn_is_reserved(pfn))
        {
            pfn++;
            continue;
        }

        unsigned int order = PMM_MAX_ORDER;
        while (order > 0)
        {
            unsigned long block_size = 1UL << order;
            if ((pfn & (block_size - 1)) == 0 && pfn + block_size <= pmm_num_pages
                && !range_overlaps_reserved_pfns(pfn, pfn + block_size))
            {
                break;
            }
            order--;
        }

        pmm_free_pages_count += (1UL << order);
        pmm_free_buddy_internal(pfn, order);
        pfn += (1UL << order);
    }

    pmm_managed_pages = pmm_free_pages_count;
    pmm_ready = 1;
    printf("[  PMM ] %lu MB free — buddy system ready\n", (pmm_free_pages_count * PAGE_SIZE) / (1024UL * 1024));
}

void* pmm_alloc_pages(unsigned long count)
{
    if (count == 0)
        return NULL;

    unsigned int target_order = get_order(count);
    if (target_order > PMM_MAX_ORDER)
        return NULL;

    unsigned long irq = spin_lock_irqsave(&pmm_lock);

    unsigned int current_order = target_order;
    while (current_order <= PMM_MAX_ORDER && !pmm_free_lists[current_order])
        current_order++;

    if (current_order > PMM_MAX_ORDER)
    {
        spin_unlock_irqrestore(&pmm_lock, irq);
        return NULL;
    }

    struct pmm_page* p = pmm_free_lists[current_order];
    pmm_free_lists[current_order] = p->next;
    p->is_free = 0;
    p->next = NULL;
    pmm_free_pages_count -= (1UL << current_order);

    unsigned long pfn = page_to_pfn(p);

    // Split down to target order
    while (current_order > target_order)
    {
        current_order--;
        unsigned long buddy_pfn = pfn + (1UL << current_order);
        struct pmm_page* buddy = pfn_to_page(buddy_pfn);

        buddy->is_free = 1;
        buddy->order = (uint8_t)current_order;
        buddy->refcount = 0;
        buddy->next = pmm_free_lists[current_order];
        pmm_free_lists[current_order] = buddy;
        pmm_free_pages_count += (1UL << current_order);
    }

    p->order = (uint8_t)target_order;
    p->refcount = 1;

    spin_unlock_irqrestore(&pmm_lock, irq);

    void* vaddr = (void*)P2V(pfn * PAGE_SIZE);
    memset(vaddr, 0, (size_t)(1UL << target_order) * PAGE_SIZE);

    return vaddr;
}

void pmm_free_pages(void* ptr, unsigned long count)
{
    (void)count;
    if (!ptr)
        return;

    unsigned long pfn = V2P((unsigned long)ptr) / PAGE_SIZE;
    if (pfn >= pmm_num_pages)
        PANIC("PMM: free out of range");

    if (pfn_is_reserved(pfn))
        return;

    unsigned long irq = spin_lock_irqsave(&pmm_lock);
    struct pmm_page* p = pfn_to_page(pfn);

    if (p->is_free)
    {
        spin_unlock_irqrestore(&pmm_lock, irq);
        PANIC("PMM: double-free");
    }
    if (p->refcount == 0)
    {
        spin_unlock_irqrestore(&pmm_lock, irq);
        PANIC("PMM: free refcount 0");
    }

    p->refcount--;
    if (p->refcount > 0)
    {
        spin_unlock_irqrestore(&pmm_lock, irq);
        return;
    }

    unsigned int order = p->order;
    if (order > PMM_MAX_ORDER)
    {
        spin_unlock_irqrestore(&pmm_lock, irq);
        PANIC("PMM: corrupt page order");
    }

    memset(ptr, 0, (size_t)(1UL << order) * PAGE_SIZE);
    pmm_free_pages_count += (1UL << order);
    pmm_free_buddy_internal(pfn, order);

    spin_unlock_irqrestore(&pmm_lock, irq);
}

void pmm_hold_page(void* ptr)
{
    if (!ptr)
        return;

    unsigned long pfn = V2P((unsigned long)ptr) / PAGE_SIZE;
    if (pfn >= pmm_num_pages || pfn_is_reserved(pfn))
        return;

    unsigned long irq = spin_lock_irqsave(&pmm_lock);
    struct pmm_page* p = &pmm_page_array[pfn];

    if (p->is_free)
    {
        spin_unlock_irqrestore(&pmm_lock, irq);
        PANIC("PMM: hold on free page");
    }

    if (p->refcount == (uint16_t)-1)
    {
        spin_unlock_irqrestore(&pmm_lock, irq);
        PANIC("PMM: refcount overflow");
    }

    p->refcount++;
    spin_unlock_irqrestore(&pmm_lock, irq);
}

int pmm_is_managed(void* ptr)
{
    if (!ptr)
        return 0;
    unsigned long pfn = V2P((unsigned long)ptr) / PAGE_SIZE;
    return pfn < pmm_num_pages && !pfn_is_reserved(pfn);
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
    return pmm_free_pages_count;
}

unsigned long pmm_get_total_pages(void)
{
    return pmm_managed_pages;
}
