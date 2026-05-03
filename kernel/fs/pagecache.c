/*
 * pagecache.c - Implementation of the Unified Page Cache Layer.
 *
 * This module provides an in-memory cache for 4KB virtual file pages.
 * Filesystem drivers use this layer to accelerate file I/O operations.
 */

#include "fs/pagecache.h"

#include "stdio.h"
#include "string.h"

#include "uapi/errors.h"

#include "mm/pmm.h"
#include "mm/slab.h"
#include "core/lock.h"

#define PAGECACHE_HASH_SIZE 256
#define PAGECACHE_MAX_PAGES 1024 /* 4MB of page cache */

struct page_cache_entry {
    struct vfs_vnode *vnode;
    void *fs_ops;
    void *file_id;
    size_t page_index;
    void *data;
    int dirty;
    struct page_cache_entry *next;
    struct page_cache_entry *lru_next;
    struct page_cache_entry *lru_prev;
};

static struct page_cache_entry *hash_table[PAGECACHE_HASH_SIZE];
static struct page_cache_entry *lru_head = NULL;
static struct page_cache_entry *lru_tail = NULL;
static size_t cache_count = 0;
static spinlock_t pagecache_lock = SPINLOCK_INIT;

static size_t page_hash(struct vfs_vnode *node, size_t page_index)
{
    return ((uintptr_t)node->ops + (uintptr_t)node->internal_info + page_index)
           % PAGECACHE_HASH_SIZE;
}

static void lru_remove(struct page_cache_entry *entry)
{
    if (entry->lru_prev) {
        entry->lru_prev->lru_next = entry->lru_next;
    } else {
        lru_head = entry->lru_next;
    }

    if (entry->lru_next) {
        entry->lru_next->lru_prev = entry->lru_prev;
    } else {
        lru_tail = entry->lru_prev;
    }

    entry->lru_next = entry->lru_prev = NULL;
}

static void lru_add_head(struct page_cache_entry *entry)
{
    entry->lru_next = lru_head;
    entry->lru_prev = NULL;
    if (lru_head) {
        lru_head->lru_prev = entry;
    }
    lru_head = entry;
    if (!lru_tail) {
        lru_tail = entry;
    }
}

void pagecache_init(void)
{
    pr_info("pagecache: initialized with capacity for %d pages\n", PAGECACHE_MAX_PAGES);
}

void *pagecache_get_page(struct vfs_vnode *node, size_t page_index)
{
    unsigned long flags = spin_lock_irqsave(&pagecache_lock);
    size_t h = page_hash(node, page_index);
    struct page_cache_entry *curr = hash_table[h];

    while (curr) {
        if (curr->fs_ops == node->ops && curr->file_id == node->internal_info
            && curr->page_index == page_index) {
            lru_remove(curr);
            lru_add_head(curr);
            void *data = curr->data;
            spin_unlock_irqrestore(&pagecache_lock, flags);
            return data;
        }
        curr = curr->next;
    }

    spin_unlock_irqrestore(&pagecache_lock, flags);
    return NULL;
}

int pagecache_add_page(struct vfs_vnode *node, size_t page_index, void *data)
{
    unsigned long flags = spin_lock_irqsave(&pagecache_lock);

    /* Avoid duplicate entries */
    size_t h = page_hash(node, page_index);
    struct page_cache_entry *curr = hash_table[h];
    while (curr) {
        if (curr->fs_ops == node->ops && curr->file_id == node->internal_info
            && curr->page_index == page_index) {
            spin_unlock_irqrestore(&pagecache_lock, flags);
            return -PERS_ERR_ALREADY_EXISTS;
        }
        curr = curr->next;
    }

    struct page_cache_entry *entry;

    if (cache_count >= PAGECACHE_MAX_PAGES) {
        entry = lru_tail;
        lru_remove(entry);

        /* Write back dirty page before eviction */
        if (entry->dirty && entry->vnode && entry->vnode->ops && entry->vnode->ops->write_page) {
            spin_unlock_irqrestore(&pagecache_lock, flags);
            entry->vnode->ops->write_page(entry->vnode, entry->page_index, entry->data, PAGE_SIZE);
            flags = spin_lock_irqsave(&pagecache_lock);
        }

        size_t eh = ((uintptr_t)entry->fs_ops + (uintptr_t)entry->file_id + entry->page_index)
                    % PAGECACHE_HASH_SIZE;
        struct page_cache_entry **pp = &hash_table[eh];
        while (*pp && *pp != entry) {
            pp = &((*pp)->next);
        }
        if (*pp == entry) {
            *pp = entry->next;
        }

        /* Reclaim the old page's physical memory */
        pmm_free_pages(entry->data, 1);
    } else {
        entry = slab_alloc(sizeof(struct page_cache_entry));
        if (!entry) {
            spin_unlock_irqrestore(&pagecache_lock, flags);
            return -PERS_ERR_OUT_OF_MEMORY;
        }
        cache_count++;
    }

    entry->vnode = node;
    entry->fs_ops = node->ops;
    entry->file_id = node->internal_info;
    entry->page_index = page_index;
    entry->data = data;
    entry->dirty = 0;

    entry->next = hash_table[h];
    hash_table[h] = entry;
    lru_add_head(entry);

    spin_unlock_irqrestore(&pagecache_lock, flags);
    return PERS_SUCCESS;
}

void pagecache_mark_dirty(struct vfs_vnode *node, size_t page_index)
{
    unsigned long flags = spin_lock_irqsave(&pagecache_lock);
    size_t h = page_hash(node, page_index);
    struct page_cache_entry *curr = hash_table[h];

    while (curr) {
        if (curr->fs_ops == node->ops && curr->file_id == node->internal_info
            && curr->page_index == page_index) {
            curr->dirty = 1;
            break;
        }
        curr = curr->next;
    }

    spin_unlock_irqrestore(&pagecache_lock, flags);
}

void pagecache_clear_dirty(struct vfs_vnode *node, size_t page_index)
{
    unsigned long flags = spin_lock_irqsave(&pagecache_lock);
    size_t h = page_hash(node, page_index);
    struct page_cache_entry *curr = hash_table[h];

    while (curr) {
        if (curr->fs_ops == node->ops && curr->file_id == node->internal_info
            && curr->page_index == page_index) {
            curr->dirty = 0;
            break;
        }
        curr = curr->next;
    }

    spin_unlock_irqrestore(&pagecache_lock, flags);
}

int pagecache_writeback(struct vfs_vnode *node)
{
    if (!node || !node->ops || !node->ops->write_page) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    int pages_written = 0;
    unsigned long flags = spin_lock_irqsave(&pagecache_lock);

    for (size_t h = 0; h < PAGECACHE_HASH_SIZE; h++) {
        struct page_cache_entry *curr = hash_table[h];
        while (curr) {
            if (curr->dirty && curr->fs_ops == node->ops && curr->file_id == node->internal_info) {
                struct page_cache_entry *to_write = curr;
                curr = curr->next;

                spin_unlock_irqrestore(&pagecache_lock, flags);

                int result =
                    node->ops->write_page(node, to_write->page_index, to_write->data, PAGE_SIZE);

                flags = spin_lock_irqsave(&pagecache_lock);

                if (result >= 0) {
                    to_write->dirty = 0;
                    pages_written++;
                }
            } else {
                curr = curr->next;
            }
        }
    }

    spin_unlock_irqrestore(&pagecache_lock, flags);
    return pages_written;
}

int pagecache_sync(void)
{
    int total_written = 0;
    unsigned long flags = spin_lock_irqsave(&pagecache_lock);

    for (size_t h = 0; h < PAGECACHE_HASH_SIZE; h++) {
        struct page_cache_entry *curr = hash_table[h];
        while (curr) {
            if (curr->dirty && curr->vnode && curr->vnode->ops && curr->vnode->ops->write_page) {
                struct page_cache_entry *to_write = curr;
                struct vfs_vnode *vnode = to_write->vnode;
                curr = curr->next;

                spin_unlock_irqrestore(&pagecache_lock, flags);

                int result =
                    vnode->ops->write_page(vnode, to_write->page_index, to_write->data, PAGE_SIZE);

                flags = spin_lock_irqsave(&pagecache_lock);

                if (result >= 0) {
                    to_write->dirty = 0;
                    total_written++;
                }
            } else {
                curr = curr->next;
            }
        }
    }

    spin_unlock_irqrestore(&pagecache_lock, flags);
    return total_written;
}

void pagecache_invalidate(struct vfs_vnode *node)
{
    unsigned long flags = spin_lock_irqsave(&pagecache_lock);

    for (size_t h = 0; h < PAGECACHE_HASH_SIZE; h++) {
        struct page_cache_entry **pp = &hash_table[h];
        while (*pp) {
            struct page_cache_entry *entry = *pp;
            if (entry->fs_ops == node->ops && entry->file_id == node->internal_info) {
                *pp = entry->next;
                lru_remove(entry);

                /* Write back if dirty before invalidating */
                if (entry->dirty && node->ops && node->ops->write_page) {
                    spin_unlock_irqrestore(&pagecache_lock, flags);
                    node->ops->write_page(node, entry->page_index, entry->data, PAGE_SIZE);
                    flags = spin_lock_irqsave(&pagecache_lock);
                }

                pmm_free_pages(entry->data, 1);
                slab_free(entry);
                cache_count--;
            } else {
                pp = &((*pp)->next);
            }
        }
    }

    spin_unlock_irqrestore(&pagecache_lock, flags);
}
