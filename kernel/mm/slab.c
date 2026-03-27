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

/* --- Private Macros --- */

#define SLAB_MAGIC       0x534C4142U
#define SLAB_FREE_POISON 0xDEADBEEFDEADBEEFULL
#define SLAB_NUM_CLASSES 7

/* --- Private Data Structures --- */

struct slab_obj {
    struct slab_obj *next;
    uint64_t free_canary;
};

struct slab_page {
    unsigned int magic;
    unsigned int class_idx;
    unsigned int total_slots;
    unsigned int in_use_count;
    struct slab_obj *free_list;
    struct slab_page *next;
};

struct slab_class {
    unsigned long object_size;
    struct slab_page *partial_list;
    struct slab_page *full_list;
    spinlock_t lock;
};

/* --- Private Variables --- */

static const unsigned long slab_class_sizes[SLAB_NUM_CLASSES] = {16, 32, 64, 128, 256, 512, 1024};

static struct slab_class slab_classes[SLAB_NUM_CLASSES];
static unsigned long slab_total_pages = 0;

/* --- Private Helper Functions --- */

static inline struct slab_page *ptr_to_slab(void *ptr)
{
    return (struct slab_page *)((unsigned long)ptr & ~(PAGE_SIZE - 1UL));
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

    unsigned long obj_size = sc->object_size;
    unsigned long hdr_size = sizeof(struct slab_page);
    unsigned long start_offset = (hdr_size + obj_size - 1) & ~(obj_size - 1);

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

/* --- Public API Implementations --- */

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
    sp->free_list = obj->next;
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

    if (obj->free_canary == SLAB_FREE_POISON) {
        PANIC("slab: double free detected");
    }

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
