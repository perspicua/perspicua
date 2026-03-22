/*
 * slab.c - Implementation of the size-class slab allocator.
 *
 * This file implements a per-page slab allocator for small objects.
 * It manages multiple size classes to reduce fragmentation and uses
 * per-class spinlocks to minimize contention.
 *
 * Lock ordering: slab class lock must always be acquired before the PMM
 * lock. slab_grow() calls pmm_alloc_page() while holding the class lock,
 * establishing this order. No code path may acquire a class lock while
 * the PMM lock is held.
 */

#include "mm/slab.h"

#include "mm/pmm.h"
#include "core/lock.h"
#include "stdio.h"
#include "panic.h"

/* Magic number for validating slab pages */
#define SLAB_MAGIC 0x534C4142U

/*
 * Poison value written into the free_canary field of every free slot.
 * Detects simple double-frees where the caller has not written to the
 * object after freeing it. Use-after-free that clobbers the canary will
 * defeat this check — it is a best-effort guard, not a guarantee.
 */
#define SLAB_FREE_POISON 0xDEADBEEFDEADBEEFULL

/* Total number of supported size classes */
#define SLAB_NUM_CLASSES 7

/*
 * All object sizes must be powers of two. slab_grow() uses bitwise
 * alignment which is only correct under this constraint. A static assert
 * in slab_init() verifies this for every class at boot.
 */
static const unsigned long slab_class_sizes[SLAB_NUM_CLASSES] = {16, 32, 64, 128, 256, 512, 1024};

/*
 * slab_obj - A free-list node embedded directly into unallocated slots.
 */
struct slab_obj
{
    struct slab_obj* next;
    uint64_t free_canary;
};

/*
 * slab_page - Header structure located at the start of every physical page
 * managed by the slab allocator.
 */
struct slab_page
{
    unsigned int magic;
    unsigned int class_idx;
    unsigned int total_slots;
    unsigned int in_use_count;
    struct slab_obj* free_list;
    struct slab_page* next;
};

/*
 * slab_class - Metadata for a specific size class pool.
 */
struct slab_class
{
    unsigned long object_size;
    struct slab_page* partial_list;
    struct slab_page* full_list;
    spinlock_t lock;
};

/* Internal array of slab class descriptors */
static struct slab_class slab_classes[SLAB_NUM_CLASSES];

/* Total number of PMM pages currently held by the slab allocator */
static unsigned long slab_total_pages = 0;

/*
 * ptr_to_slab - Recovers the slab page header from a pointer to an
 * object within that page by masking the page offset.
 */
static inline struct slab_page* ptr_to_slab(void* ptr)
{
    return (struct slab_page*)((unsigned long)ptr & ~(PAGE_SIZE - 1UL));
}

/*
 * size_to_class_index - Determines the best-fit size class index for
 * a requested allocation size. Returns -1 if the size is too large.
 */
static inline int size_to_class_index(unsigned long size)
{
    for (int i = 0; i < SLAB_NUM_CLASSES; i++)
    {
        if (size <= slab_class_sizes[i])
        {
            return i;
        }
    }
    return -1;
}

/*
 * slab_grow - Allocates a fresh physical page from the PMM, initializes
 * its header, and carves it into fixed-size slots for the specified class.
 *
 * Called with the class lock held. pmm_alloc_page() acquires the PMM lock
 * internally, which is always taken after the class lock — see file header.
 */
static struct slab_page* slab_grow(struct slab_class* sc, unsigned int idx)
{
    void* page = pmm_alloc_page();
    if (!page)
    {
        return NULL;
    }

    struct slab_page* sp = (struct slab_page*)page;
    sp->magic = SLAB_MAGIC;
    sp->class_idx = idx;
    sp->in_use_count = 0;
    sp->free_list = NULL;

    unsigned long obj_size = sc->object_size;
    unsigned long hdr_size = sizeof(struct slab_page);

    /*
     * Round the header up to the next multiple of obj_size so that every
     * slot is naturally aligned. This relies on obj_size being a power of
     * two, which is enforced by the assertion in slab_init().
     */
    unsigned long start_offset = (hdr_size + obj_size - 1) & ~(obj_size - 1);

    unsigned int count = 0;
    for (unsigned long off = start_offset; off + obj_size <= PAGE_SIZE; off += obj_size)
    {
        struct slab_obj* obj = (struct slab_obj*)((unsigned char*)page + off);
        obj->next = sp->free_list;
        obj->free_canary = SLAB_FREE_POISON;
        sp->free_list = obj;
        count++;
    }

    if (count == 0)
    {
        /*
         * The object size is so large that the header leaves no room for
         * any slots. This should never happen with the current size classes
         * and PAGE_SIZE, but return the page and signal failure cleanly
         * rather than handing back a header-only slab.
         */
        pmm_free_page(page);
        return NULL;
    }

    sp->total_slots = count;
    slab_total_pages++;

    /* Prepend the new page to the partial list since it has free slots */
    sp->next = sc->partial_list;
    sc->partial_list = sp;
    return sp;
}

/*
 * slab_release_page - Returns an entirely empty slab page to the PMM.
 *
 * The page must already be unlinked from its class list before this is
 * called. Called with the class lock held; pmm_free_page() acquires the
 * PMM lock internally.
 */
static void slab_release_page(struct slab_page* sp)
{
    slab_total_pages--;
    pmm_free_page((void*)sp);
}

/*
 * slab_init - Boot-time initialization of the slab allocator.
 */
void slab_init(void)
{
    /* Verify the power-of-two constraint on every size class */
    for (int i = 0; i < SLAB_NUM_CLASSES; i++)
    {
        unsigned long sz = slab_class_sizes[i];
        if (sz == 0 || (sz & (sz - 1)) != 0)
        {
            PANIC("SLAB: Size class is not a power of two");
        }
    }

    for (int i = 0; i < SLAB_NUM_CLASSES; i++)
    {
        slab_classes[i].object_size = slab_class_sizes[i];
        slab_classes[i].partial_list = NULL;
        slab_classes[i].full_list = NULL;
        slab_classes[i].lock = (spinlock_t)SPINLOCK_INIT;
    }

    /* Seed every size class with one page to avoid early allocation failures */
    for (int i = 0; i < SLAB_NUM_CLASSES; i++)
    {
        if (!slab_grow(&slab_classes[i], (unsigned int)i))
        {
            PANIC("SLAB: Failed to grow initial class pool");
        }
    }

    printf("[ SLAB ] Initialized %d size classes from 16 to 1024 bytes\n", SLAB_NUM_CLASSES);
}

/*
 * slab_alloc - Allocates an object of at least 'size' bytes.
 * Returns NULL on failure; never panics.
 */
void* slab_alloc(unsigned long size)
{
    int idx = size_to_class_index(size);
    if (idx < 0)
    {
        return NULL;
    }

    struct slab_class* sc = &slab_classes[idx];
    unsigned long flags = spin_lock_irqsave(&sc->lock);

    struct slab_page* sp = sc->partial_list;

    /* Grow the class if all current pages are full */
    if (!sp)
    {
        sp = slab_grow(sc, (unsigned int)idx);
        if (!sp)
        {
            spin_unlock_irqrestore(&sc->lock, flags);
            return NULL;
        }
    }

    /* Pop an object from the page's free list */
    struct slab_obj* obj = sp->free_list;
    sp->free_list = obj->next;
    obj->free_canary = 0;
    sp->in_use_count++;

    /* Move the page to the full list if no more slots are available */
    if (!sp->free_list)
    {
        sc->partial_list = sp->next;
        sp->next = sc->full_list;
        sc->full_list = sp;
    }

    spin_unlock_irqrestore(&sc->lock, flags);
    return (void*)obj;
}

/*
 * slab_free - Deallocates an object and returns its slot to the owner page.
 */
void slab_free(void* ptr)
{
    if (!ptr)
    {
        return;
    }

    struct slab_page* sp = ptr_to_slab(ptr);
    if (sp->magic != SLAB_MAGIC)
    {
        PANIC("SLAB: Attempted to free a non-slab pointer");
    }

    if (sp->class_idx >= SLAB_NUM_CLASSES)
    {
        PANIC("SLAB: Corrupt class index in slab header");
    }

    struct slab_class* sc = &slab_classes[sp->class_idx];
    unsigned long flags = spin_lock_irqsave(&sc->lock);

    struct slab_obj* obj = (struct slab_obj*)ptr;
    if (obj->free_canary == SLAB_FREE_POISON)
    {
        PANIC("SLAB: Double-free detected");
    }

    int was_full = (sp->free_list == NULL);

    /* Push the slot back onto the free list and poison its canary */
    obj->free_canary = SLAB_FREE_POISON;
    obj->next = sp->free_list;
    sp->free_list = obj;
    sp->in_use_count--;

    if (was_full)
    {
        /*
         * Unlink from full_list. O(n) in the number of full pages for this
         * class — unavoidable with a singly-linked list.
         */
        struct slab_page** prev = &sc->full_list;
        while (*prev && *prev != sp)
        {
            prev = &(*prev)->next;
        }
        if (!*prev)
        {
            PANIC("SLAB: Page marked full but not found in full_list");
        }
        *prev = sp->next;

        /* Return completely empty pages to the PMM immediately */
        if (sp->in_use_count == 0)
        {
            spin_unlock_irqrestore(&sc->lock, flags);
            slab_release_page(sp);
            return;
        }

        sp->next = sc->partial_list;
        sc->partial_list = sp;
    }
    else if (sp->in_use_count == 0)
    {
        /*
         * Page was already on the partial list and is now completely empty.
         * Unlink and return it to the PMM.
         */
        struct slab_page** prev = &sc->partial_list;
        while (*prev && *prev != sp)
        {
            prev = &(*prev)->next;
        }
        if (!*prev)
        {
            PANIC("SLAB: Page not found in partial_list during empty reclaim");
        }
        *prev = sp->next;

        spin_unlock_irqrestore(&sc->lock, flags);
        slab_release_page(sp);
        return;
    }

    spin_unlock_irqrestore(&sc->lock, flags);
}

/*
 * slab_owns - Returns non-zero if ptr belongs to a slab-managed page.
 */
int slab_owns(void* ptr)
{
    if (!ptr)
    {
        return 0;
    }
    struct slab_page* sp = ptr_to_slab(ptr);
    if (!pmm_is_managed((void*)sp))
    {
        return 0;
    }
    return sp->magic == SLAB_MAGIC;
}

/*
 * slab_get_used - Returns total bytes used across all size classes.
 */
unsigned long slab_get_used(void)
{
    unsigned long used = 0;
    for (int i = 0; i < SLAB_NUM_CLASSES; i++)
    {
        unsigned long flags = spin_lock_irqsave(&slab_classes[i].lock);

        struct slab_page* sp = slab_classes[i].partial_list;
        while (sp)
        {
            used += sp->in_use_count * slab_classes[i].object_size;
            sp = sp->next;
        }

        sp = slab_classes[i].full_list;
        while (sp)
        {
            used += sp->in_use_count * slab_classes[i].object_size;
            sp = sp->next;
        }

        spin_unlock_irqrestore(&slab_classes[i].lock, flags);
    }
    return used;
}

/*
 * slab_get_total - Returns total capacity across all allocated slab pages.
 */
unsigned long slab_get_total(void)
{
    return slab_total_pages * PAGE_SIZE;
}
