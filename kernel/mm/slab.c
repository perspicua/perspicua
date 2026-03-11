#include "slab.h"
#include "pmm.h"
#include "lock.h"
#include "stdio.h"
#include "panic.h"

// ---------------------------------------------------------------------------
// Size-class slab allocator
//
// Each size class owns a pool of PAGE_SIZE slabs.  Every slab is carved into
// fixed-size slots.  A per-slot freelist provides O(1) alloc/free with zero
// per-object header overhead.  Each size class has its own spinlock so
// different-class allocations never contend.
//
// Object -> slab lookup: mask off the low 12 bits of any pointer to get the
// slab page, then read the slab_page header at the top of that page.
// ---------------------------------------------------------------------------

#define SLAB_MAGIC 0x534C4142U                 // "SLAB"
#define SLAB_FREE_POISON 0xDEADBEEFDEADBEEFULL // written to every free slot

// size classes: 16, 32, 64, 128, 256, 512, 1024
#define NUM_CLASSES 7
static const unsigned long class_size[NUM_CLASSES] = {16, 32, 64, 128, 256, 512, 1024};

// free-list node embedded inside every free slot
// NOTE: sizeof(slab_obj) == 16, which fits exactly in the smallest class (16B)
struct slab_obj
{
    struct slab_obj* next;
    uint64_t free_canary; // SLAB_FREE_POISON when slot is free
};

// per-page header living at the start of every slab page
struct slab_page
{
    unsigned int magic;     // SLAB_MAGIC — used by slab_owns()
    unsigned int class_idx; // which size class this page belongs to
    unsigned int total;     // total slots in this page
    unsigned int in_use;    // currently allocated slots
    struct slab_obj* free;  // per-page freelist head
    struct slab_page* next; // next slab page in this class
};

// per-class metadata
struct slab_class
{
    unsigned long obj_size;    // slot size (bytes)
    struct slab_page* partial; // pages with free slots (alloc pops head → O(1))
    struct slab_page* full;    // pages with no free slots
    spinlock_t lock;
};

static struct slab_class classes[NUM_CLASSES];

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// given a pointer inside a slab page, recover the slab_page header
static inline struct slab_page* ptr_to_slab(void* ptr)
{
    return (struct slab_page*)((unsigned long)ptr & ~(PAGE_SIZE - 1UL));
}

// return the class index for a given size, or -1 if too large
static inline int size_to_class(unsigned long size)
{
    for (int i = 0; i < NUM_CLASSES; i++)
        if (size <= class_size[i])
            return i;
    return -1;
}

// allocate a new slab page for the given class and prepend to partial list
static struct slab_page* slab_grow(struct slab_class* sc, unsigned int idx)
{
    void* page = pmm_alloc_page();
    if (!page)
        return 0;

    struct slab_page* sp = (struct slab_page*)page;
    sp->magic = SLAB_MAGIC;
    sp->class_idx = idx;
    sp->in_use = 0;
    sp->free = 0;

    unsigned long obj_size = sc->obj_size;
    // first slot starts right after the header, aligned to obj_size
    unsigned long hdr = sizeof(struct slab_page);
    unsigned long start = (hdr + obj_size - 1) & ~(obj_size - 1);

    unsigned int count = 0;
    for (unsigned long off = start; off + obj_size <= PAGE_SIZE; off += obj_size)
    {
        struct slab_obj* obj = (struct slab_obj*)((unsigned char*)page + off);
        obj->next = sp->free;
        obj->free_canary = SLAB_FREE_POISON;
        sp->free = obj;
        count++;
    }
    sp->total = count;

    // new page has free slots → partial list
    sp->next = sc->partial;
    sc->partial = sp;
    return sp;
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

void slab_init(void)
{
    for (int i = 0; i < NUM_CLASSES; i++)
    {
        classes[i].obj_size = class_size[i];
        classes[i].partial = 0;
        classes[i].full = 0;
        classes[i].lock = (spinlock_t)SPINLOCK_INIT;
    }

    // pre-allocate one slab page per class so early allocs don't fault
    for (int i = 0; i < NUM_CLASSES; i++)
    {
        if (!slab_grow(&classes[i], (unsigned int)i))
            PANIC("SLAB: Failed to grow class \n");
    }

    printf("[ SLAB ] %d size classes:", NUM_CLASSES);
    for (int i = 0; i < NUM_CLASSES; i++)
        printf(" %lu", class_size[i]);
    printf(" bytes\n");
    printf("[ SLAB ] Per-class spinlock, O(1) alloc/free, zero per-object overhead\n");
}

void* slab_alloc(unsigned long size)
{
    int idx = size_to_class(size);
    if (idx < 0)
        return 0; // too large for slab

    struct slab_class* sc = &classes[idx];
    unsigned long flags = spin_lock_irqsave(&sc->lock);

    // O(1): grab head of partial list
    struct slab_page* sp = sc->partial;

    // no partial pages — grow
    if (!sp)
    {
        sp = slab_grow(sc, (unsigned int)idx);
        if (!sp)
        {
            spin_unlock_irqrestore(&sc->lock, flags);
            PANIC("SLAB: Out of memory for class \n");
            return 0;
        }
    }

    // pop from freelist
    struct slab_obj* obj = sp->free;
    sp->free = obj->next;
    obj->free_canary = 0; // clear poison — slot is now live
    sp->in_use++;

    // if page is now full, move it from partial → full
    if (!sp->free)
    {
        sc->partial = sp->next;
        sp->next = sc->full;
        sc->full = sp;
    }

    spin_unlock_irqrestore(&sc->lock, flags);
    return (void*)obj;
}

void slab_free(void* ptr)
{
    if (!ptr)
        return;

    struct slab_page* sp = ptr_to_slab(ptr);
    if (sp->magic != SLAB_MAGIC)
        PANIC("SLAB: slab_free on non-slab pointer \n");

    struct slab_class* sc = &classes[sp->class_idx];
    unsigned long flags = spin_lock_irqsave(&sc->lock);

    struct slab_obj* obj = (struct slab_obj*)ptr;
    if (obj->free_canary == SLAB_FREE_POISON)
        PANIC("SLAB: double free detected\n");

    int was_full = (sp->free == 0);

    // push back onto freelist
    obj->free_canary = SLAB_FREE_POISON;
    obj->next = sp->free;
    sp->free = obj;
    sp->in_use--;

    // if page was full, move it from full → partial
    if (was_full)
    {
        // unlink from full list
        struct slab_page** prev = &sc->full;
        while (*prev && *prev != sp)
            prev = &(*prev)->next;
        if (*prev)
            *prev = sp->next;

        // prepend to partial list
        sp->next = sc->partial;
        sc->partial = sp;
    }

    spin_unlock_irqrestore(&sc->lock, flags);
}

int slab_owns(void* ptr)
{
    if (!ptr)
        return 0;
    struct slab_page* sp = ptr_to_slab(ptr);
    return sp->magic == SLAB_MAGIC;
}
