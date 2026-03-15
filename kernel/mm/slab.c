/*
 * slab.c - Implementation of the size-class slab allocator.
 *
 * This file implements a per-page slab allocator for small objects.
 * It manages multiple size classes to reduce fragmentation and uses
 * per-class spinlocks to minimize contention.
 */

#include "slab.h"

#include "pmm.h"
#include "lock.h"
#include "stdio.h"
#include "panic.h"

/* Magic number for validating slab pages */
#define SLAB_MAGIC 0x534C4142U

/* Poison value used to detect double-frees and identify unallocated slots */
#define SLAB_FREE_POISON 0xDEADBEEFDEADBEEFULL

/* Total number of supported size classes */
#define SLAB_NUM_CLASSES 7

/* The available object size classes in bytes */
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
 */
static struct slab_page* slab_grow(struct slab_class* sc, unsigned int idx)
{
    void* page = pmm_alloc_page();
    if (!page)
    {
        return (void*)0;
    }

    struct slab_page* sp = (struct slab_page*)page;
    sp->magic = SLAB_MAGIC;
    sp->class_idx = idx;
    sp->in_use_count = 0;
    sp->free_list = (void*)0;

    unsigned long obj_size = sc->object_size;
    unsigned long hdr_size = sizeof(struct slab_page);
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
    sp->total_slots = count;

    /* Prepend the new page to the partial list since it has free slots */
    sp->next = sc->partial_list;
    sc->partial_list = sp;
    return sp;
}

/*
 * slab_init - Boot-time initialization of the slab allocator.
 */
void slab_init(void)
{
    for (int i = 0; i < SLAB_NUM_CLASSES; i++)
    {
        slab_classes[i].object_size = slab_class_sizes[i];
        slab_classes[i].partial_list = (void*)0;
        slab_classes[i].full_list = (void*)0;
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
 */
void* slab_alloc(unsigned long size)
{
    int idx = size_to_class_index(size);
    if (idx < 0)
    {
        return (void*)0;
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
            return (void*)0;
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

    struct slab_class* sc = &slab_classes[sp->class_idx];
    unsigned long flags = spin_lock_irqsave(&sc->lock);

    struct slab_obj* obj = (struct slab_obj*)ptr;
    if (obj->free_canary == SLAB_FREE_POISON)
    {
        PANIC("SLAB: Double-free detected");
    }

    int was_full = (sp->free_list == (void*)0);

    /* Push the slot back onto the free list and poison its canary */
    obj->free_canary = SLAB_FREE_POISON;
    obj->next = sp->free_list;
    sp->free_list = obj;
    sp->in_use_count--;

    /* If the page was previously full, move it back to the partial list */
    if (was_full)
    {
        struct slab_page** prev = &sc->full_list;
        while (*prev && *prev != sp)
        {
            prev = &(*prev)->next;
        }
        if (*prev)
        {
            *prev = sp->next;
        }

        sp->next = sc->partial_list;
        sc->partial_list = sp;
    }

    spin_unlock_irqrestore(&sc->lock, flags);
}

/*
 * slab_owns - Validates if a pointer belongs to the slab allocator.
 */
int slab_owns(void* ptr)
{
    if (!ptr)
    {
        return 0;
    }
    struct slab_page* sp = ptr_to_slab(ptr);
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
    unsigned long total = 0;
    for (int i = 0; i < SLAB_NUM_CLASSES; i++)
    {
        unsigned long flags = spin_lock_irqsave(&slab_classes[i].lock);

        struct slab_page* sp = slab_classes[i].partial_list;
        while (sp)
        {
            total += sp->total_slots * slab_classes[i].object_size;
            sp = sp->next;
        }

        sp = slab_classes[i].full_list;
        while (sp)
        {
            total += sp->total_slots * slab_classes[i].object_size;
            sp = sp->next;
        }

        spin_unlock_irqrestore(&slab_classes[i].lock, flags);
    }
    return total;
}
