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
#define MAX_ORDER 10 // up to 1024 pages (4MB blocks)

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

    for (int i = 0; i <= MAX_ORDER; i++)
        free_lists[i] = 0;

    for (unsigned long i = 0; i < num_pages; i++)
    {
        page_array[i].next = 0;
        page_array[i].order = 0;
        page_array[i].is_free = 0;
        page_array[i].refcount = 0;
    }

    printf("[  PMM ] Buddy allocator: %lu pages total, %lu reserved (kernel+metadata)\n", (unsigned long)num_pages,
           reserved_pages);
    printf("[  PMM ] Page array: %lu KB metadata at 0x%lx\n", array_size / 1024, (unsigned long)page_array);
    printf("[  PMM ] Usable range: PFN %lu..%lu, max order %d (%lu KB blocks)\n", reserved_pages,
           (unsigned long)num_pages - 1, MAX_ORDER, (unsigned long)((1UL << MAX_ORDER) * PAGE_SIZE) / 1024);

    unsigned long pfn = reserved_pages;
    while (pfn < num_pages)
    {
        unsigned int order = MAX_ORDER;
        while (order > 0 && ((pfn & ((1UL << order) - 1)) != 0 || pfn + (1UL << order) > num_pages))
        {
            order--;
        }
        __free_buddy(pfn, order);
        pfn += (1UL << order);
    }

    unsigned long free_pages = num_pages - reserved_pages;
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

    if (pfn < pmm_reserved_pages || pfn >= num_pages)
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

    if (pfn < pmm_reserved_pages || pfn >= num_pages)
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
    return num_pages;
}
