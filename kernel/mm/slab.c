/*
 * slab.c - Implementation of the size-class slab allocator.
 *
 * This module manages per-page object pools. It groups objects into
 * power-of-two size classes to reduce internal fragmentation and uses
 * per-class locks to minimize core contention.
 */

#include "mm/slab.h"

#include "stdio.h"
#include "panic.h"

#include "core/lock.h"
#include "mm/pmm.h"

#define SLAB_MAGIC       0x534C4142U
#define SLAB_FREE_POISON 0xDEADBEEFDEADBEEFULL
#define SLAB_NUM_CLASSES 7

/* A page of the smallest class holds the most objects, bounding the bitmap. */
#define SLAB_MAX_SLOTS    (PAGE_SIZE / 16)
#define SLAB_BITMAP_WORDS (SLAB_MAX_SLOTS / 64)

struct slab_obj {
    struct slab_obj *next;
    uint64_t free_canary;
};

/*
 * struct slab_page - Header of a page carved into objects of one size class.
 *
 * `used` tracks liveness out of band. An in-object marker cannot do this job:
 * the bytes of a live object belong to its owner, who may legitimately store
 * anything there, including whatever value the marker uses.
 */
struct slab_page {
    unsigned int magic;
    unsigned int class_idx;
    unsigned int total_slots;
    unsigned int in_use_count;
    struct slab_obj *free_list;
    struct slab_page *next;
    unsigned long slot_start;
    uint64_t used[SLAB_BITMAP_WORDS];
};

struct slab_class {
    unsigned long object_size;
    struct slab_page *partial_list;
    struct slab_page *full_list;
    spinlock_t lock;
};

static const unsigned long slab_class_sizes[SLAB_NUM_CLASSES] = {16, 32, 64, 128, 256, 512, 1024};

static struct slab_class slab_classes[SLAB_NUM_CLASSES];
static unsigned long slab_total_pages = 0;

static inline struct slab_page *ptr_to_slab(void *ptr)
{
    return (struct slab_page *)((unsigned long)ptr & ~(PAGE_SIZE - 1UL));
}

/*
 * slab_slot_of - Index of an object in its page, or -1 if ptr is not the start
 * of a slot in it.
 */
static int slab_slot_of(const struct slab_page *sp, const void *ptr)
{
    unsigned long base = (unsigned long)sp + sp->slot_start;
    unsigned long addr = (unsigned long)ptr;

    if (addr < base) {
        return -1;
    }

    unsigned long offset = addr - base;
    unsigned long size = slab_classes[sp->class_idx].object_size;

    if (offset % size != 0) {
        return -1;
    }

    unsigned long index = offset / size;
    if (index >= sp->total_slots) {
        return -1;
    }
    return (int)index;
}

static inline int slab_slot_is_used(const struct slab_page *sp, int slot)
{
    return (sp->used[slot / 64] >> (slot % 64)) & 1ULL;
}

static inline void slab_slot_mark(struct slab_page *sp, int slot, int used)
{
    uint64_t bit = 1ULL << (slot % 64);
    if (used) {
        sp->used[slot / 64] |= bit;
    } else {
        sp->used[slot / 64] &= ~bit;
    }
}

static inline int size_to_class_index(unsigned long size)
{
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        if (size <= slab_class_sizes[i]) {
            return i;
        }
    }
    return -1;
}

/*
 * slab_grow - Allocates and carves a new page for a specific size class.
 */
static struct slab_page *slab_grow(struct slab_class *sc, unsigned int idx)
{
    void *page = pmm_alloc_page();
    if (!page) {
        return NULL;
    }

    struct slab_page *sp = (struct slab_page *)page;
    sp->magic = SLAB_MAGIC;
    sp->class_idx = idx;
    sp->in_use_count = 0;
    sp->free_list = NULL;

    for (unsigned int i = 0; i < SLAB_BITMAP_WORDS; i++) {
        sp->used[i] = 0;
    }

    unsigned long obj_size = sc->object_size;
    unsigned long hdr_size = sizeof(struct slab_page);
    unsigned long start_offset = (hdr_size + obj_size - 1) & ~(obj_size - 1);
    sp->slot_start = start_offset;

    unsigned int count = 0;
    for (unsigned long off = start_offset; off + obj_size <= PAGE_SIZE; off += obj_size) {
        struct slab_obj *obj = (struct slab_obj *)((unsigned char *)page + off);
        obj->next = sp->free_list;
        obj->free_canary = SLAB_FREE_POISON;
        sp->free_list = obj;
        count++;
    }

    if (count == 0) {
        pmm_free_page(page);
        return NULL;
    }

    if (count > SLAB_MAX_SLOTS) {
        PANIC("slab: page holds more objects than the bitmap tracks");
    }

    sp->total_slots = count;
    slab_total_pages++;

    sp->next = sc->partial_list;
    sc->partial_list = sp;
    return sp;
}

static void slab_release_page(struct slab_page *sp)
{
    slab_total_pages--;
    pmm_free_page((void *)sp);
}

void slab_init(void)
{
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        unsigned long sz = slab_class_sizes[i];
        if (sz == 0 || (sz & (sz - 1)) != 0) {
            PANIC("slab: size class not a power of two");
        }

        slab_classes[i].object_size = sz;
        slab_classes[i].partial_list = NULL;
        slab_classes[i].full_list = NULL;
        slab_classes[i].lock = (spinlock_t)SPINLOCK_INIT;

        if (!slab_grow(&slab_classes[i], (unsigned int)i)) {
            PANIC("slab: failed to seed class pool");
        }
    }

    pr_info("slab: initialized %d classes (16 to 1024 bytes)\n", SLAB_NUM_CLASSES);
}

void *slab_alloc(unsigned long size)
{
    int idx = size_to_class_index(size);
    if (idx < 0) {
        return NULL;
    }

    struct slab_class *sc = &slab_classes[idx];
    unsigned long flags = spin_lock_irqsave(&sc->lock);
    struct slab_page *sp = sc->partial_list;

    if (!sp) {
        sp = slab_grow(sc, (unsigned int)idx);
        if (!sp) {
            spin_unlock_irqrestore(&sc->lock, flags);
            return NULL;
        }
    }

    struct slab_obj *obj = sp->free_list;
    if (!obj) {
        PANIC("slab: partial page with an empty free list");
    }

    /*
     * Nothing may write to an object while it sits on the free list, so a
     * disturbed marker here means a use-after-free. Checked on the way out
     * rather than on the way in, where the bytes belong to the caller.
     */
    if (obj->free_canary != SLAB_FREE_POISON) {
        PANIC("slab: free object was written after being freed");
    }

    int slot = slab_slot_of(sp, obj);
    if (slot < 0) {
        PANIC("slab: free list holds a misaligned object");
    }

    sp->free_list = obj->next;
    slab_slot_mark(sp, slot, 1);
    obj->free_canary = 0;
    sp->in_use_count++;

    if (!sp->free_list) {
        sc->partial_list = sp->next;
        sp->next = sc->full_list;
        sc->full_list = sp;
    }

    spin_unlock_irqrestore(&sc->lock, flags);
    return (void *)obj;
}

void slab_free(void *ptr)
{
    if (!ptr) {
        return;
    }

    struct slab_page *sp = ptr_to_slab(ptr);
    if (sp->magic != SLAB_MAGIC) {
        PANIC("slab: invalid pointer in free");
    }

    if (sp->class_idx >= SLAB_NUM_CLASSES) {
        PANIC("slab: corrupt header index");
    }

    struct slab_class *sc = &slab_classes[sp->class_idx];
    unsigned long flags = spin_lock_irqsave(&sc->lock);
    struct slab_obj *obj = (struct slab_obj *)ptr;

    int slot = slab_slot_of(sp, ptr);
    if (slot < 0) {
        spin_unlock_irqrestore(&sc->lock, flags);
        PANIC("slab: pointer is not the start of an object");
    }
    if (!slab_slot_is_used(sp, slot)) {
        spin_unlock_irqrestore(&sc->lock, flags);
        PANIC("slab: double free detected");
    }
    slab_slot_mark(sp, slot, 0);

    int was_full = (sp->free_list == NULL);
    obj->free_canary = SLAB_FREE_POISON;
    obj->next = sp->free_list;
    sp->free_list = obj;
    sp->in_use_count--;

    if (was_full) {
        struct slab_page **prev = &sc->full_list;
        while (*prev && *prev != sp) {
            prev = &(*prev)->next;
        }
        if (!*prev) {
            PANIC("slab: full page lost");
        }
        *prev = sp->next;

        if (sp->in_use_count == 0) {
            spin_unlock_irqrestore(&sc->lock, flags);
            slab_release_page(sp);
            return;
        }

        sp->next = sc->partial_list;
        sc->partial_list = sp;
    } else if (sp->in_use_count == 0) {
        struct slab_page **prev = &sc->partial_list;
        while (*prev && *prev != sp) {
            prev = &(*prev)->next;
        }
        if (!*prev) {
            PANIC("slab: partial page lost");
        }
        *prev = sp->next;

        spin_unlock_irqrestore(&sc->lock, flags);
        slab_release_page(sp);
        return;
    }

    spin_unlock_irqrestore(&sc->lock, flags);
}

int slab_owns(void *ptr)
{
    if (!ptr) {
        return 0;
    }
    struct slab_page *sp = ptr_to_slab(ptr);
    if (!pmm_is_managed((void *)sp)) {
        return 0;
    }
    return sp->magic == SLAB_MAGIC;
}

unsigned long slab_get_used(void)
{
    unsigned long used = 0;
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        unsigned long flags = spin_lock_irqsave(&slab_classes[i].lock);
        struct slab_page *sp = slab_classes[i].partial_list;

        while (sp) {
            used += sp->in_use_count * slab_classes[i].object_size;
            sp = sp->next;
        }

        sp = slab_classes[i].full_list;
        while (sp) {
            used += sp->in_use_count * slab_classes[i].object_size;
            sp = sp->next;
        }

        spin_unlock_irqrestore(&slab_classes[i].lock, flags);
    }
    return used;
}

unsigned long slab_get_total(void)
{
    return slab_total_pages * PAGE_SIZE;
}

#ifdef CONFIG_TESTS
unsigned long slab_test_object_size(void *ptr)
{
    if (!slab_owns(ptr)) {
        return 0;
    }
    return slab_classes[ptr_to_slab(ptr)->class_idx].object_size;
}
#endif
