/*
 * pagecache.c - Unified page-level memory cache for file content.
 */

#include "fs/pagecache.h"

#include <stddef.h>
#include <stdint.h>

#include "stdio.h"
#include "string.h"

#include "uapi/errno.h"

#include "mm/pmm.h"
#include "mm/slab.h"
#include "core/lock.h"

#define PAGECACHE_HASH_SIZE 256
#define PAGECACHE_MAX_PAGES 1024 // 4MB of page cache

struct page_cache_entry {
    struct vfs_vnode *vnode;
    void *fs_ops;
    void *file_id;
    size_t page_index;
    void *data;
    int dirty;
    int pincount;
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
            curr->pincount++; // Held until the caller calls pagecache_put_page
            void *data = curr->data;
            spin_unlock_irqrestore(&pagecache_lock, flags);
            return data;
        }
        curr = curr->next;
    }

    spin_unlock_irqrestore(&pagecache_lock, flags);
    return NULL;
}

void pagecache_put_page(struct vfs_vnode *node, size_t page_index)
{
    unsigned long flags = spin_lock_irqsave(&pagecache_lock);
    size_t h = page_hash(node, page_index);
    struct page_cache_entry *curr = hash_table[h];

    while (curr) {
        if (curr->fs_ops == node->ops && curr->file_id == node->internal_info
            && curr->page_index == page_index) {
            if (curr->pincount > 0) {
                curr->pincount--;
            }
            break;
        }
        curr = curr->next;
    }

    spin_unlock_irqrestore(&pagecache_lock, flags);
}

int pagecache_add_page(struct vfs_vnode *node, size_t page_index, void *data)
{
    unsigned long flags = spin_lock_irqsave(&pagecache_lock);

    // Avoid duplicate entries
    size_t h = page_hash(node, page_index);
    struct page_cache_entry *curr = hash_table[h];
    while (curr) {
        if (curr->fs_ops == node->ops && curr->file_id == node->internal_info
            && curr->page_index == page_index) {
            spin_unlock_irqrestore(&pagecache_lock, flags);
            return -EEXIST;
        }
        curr = curr->next;
    }

    // Evict the least-recently-used UNPINNED entry when the cache is full.
    struct page_cache_entry *entry = NULL;
    if (cache_count >= PAGECACHE_MAX_PAGES) {
        for (struct page_cache_entry *v = lru_tail; v; v = v->lru_prev) {
            if (v->pincount == 0) {
                entry = v;
                break;
            }
        }
    }

    if (entry) {
        lru_remove(entry);

        /* Unhook from the hash before any unlocked writeback so a concurrent
         * lookup cannot find and pin a page that is being evicted. */
        size_t eh = ((uintptr_t)entry->fs_ops + (uintptr_t)entry->file_id + entry->page_index)
                    % PAGECACHE_HASH_SIZE;
        struct page_cache_entry **pp = &hash_table[eh];
        while (*pp && *pp != entry) {
            pp = &((*pp)->next);
        }
        if (*pp == entry) {
            *pp = entry->next;
        }

        // Write back dirty page before reclaiming it
        if (entry->dirty && entry->vnode && entry->vnode->ops && entry->vnode->ops->write_page) {
            spin_unlock_irqrestore(&pagecache_lock, flags);
            entry->vnode->ops->write_page(entry->vnode, entry->page_index, entry->data, PAGE_SIZE);
            flags = spin_lock_irqsave(&pagecache_lock);
        }

        pmm_free_pages(entry->data);
    } else {
        // Under cap, or every entry is currently pinned: allocate a new slot.
        entry = slab_alloc(sizeof(struct page_cache_entry));
        if (!entry) {
            spin_unlock_irqrestore(&pagecache_lock, flags);
            return -ENOMEM;
        }
        cache_count++;
    }

    entry->vnode = node;
    entry->fs_ops = node->ops;
    entry->file_id = node->internal_info;
    entry->page_index = page_index;
    entry->data = data;
    entry->dirty = 0;
    entry->pincount = 1; // Caller holds a pin until pagecache_put_page

    entry->next = hash_table[h];
    hash_table[h] = entry;
    lru_add_head(entry);

    spin_unlock_irqrestore(&pagecache_lock, flags);
    return 0;
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
        return -EINVAL;
    }

    int pages_written = 0;
    unsigned long flags = spin_lock_irqsave(&pagecache_lock);

    for (size_t h = 0; h < PAGECACHE_HASH_SIZE; h++) {
        struct page_cache_entry *curr = hash_table[h];
        while (curr) {
            if (curr->dirty && curr->fs_ops == node->ops && curr->file_id == node->internal_info) {
                // A pinned entry is skipped by eviction and invalidation, so
                // curr and its chain link survive the unlock.
                curr->pincount++;
                spin_unlock_irqrestore(&pagecache_lock, flags);

                int result = node->ops->write_page(node, curr->page_index, curr->data, PAGE_SIZE);

                flags = spin_lock_irqsave(&pagecache_lock);

                if (result >= 0) {
                    curr->dirty = 0;
                    pages_written++;
                }

                struct page_cache_entry *next = curr->next;
                curr->pincount--;
                curr = next;
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
                struct vfs_vnode *vnode = curr->vnode;

                curr->pincount++;
                spin_unlock_irqrestore(&pagecache_lock, flags);

                int result = vnode->ops->write_page(vnode, curr->page_index, curr->data, PAGE_SIZE);

                flags = spin_lock_irqsave(&pagecache_lock);

                if (result >= 0) {
                    curr->dirty = 0;
                    total_written++;
                }

                struct page_cache_entry *next = curr->next;
                curr->pincount--;
                curr = next;
            } else {
                curr = curr->next;
            }
        }
    }

    spin_unlock_irqrestore(&pagecache_lock, flags);
    return total_written;
}

/*
 * unhook_matching - Detaches every unpinned page of one file.
 *
 * Caller holds pagecache_lock. The result is chained through ->next and is off
 * the hash and the LRU, so the caller may write it back with the lock dropped.
 */
static struct page_cache_entry *unhook_matching(void *fs_ops, void *file_id)
{
    struct page_cache_entry *doomed = NULL;

    for (size_t h = 0; h < PAGECACHE_HASH_SIZE; h++) {
        struct page_cache_entry **pp = &hash_table[h];
        while (*pp) {
            struct page_cache_entry *entry = *pp;

            // Skip other files' pages and any page currently pinned in use.
            if (entry->fs_ops != fs_ops || entry->file_id != file_id || entry->pincount != 0) {
                pp = &((*pp)->next);
                continue;
            }

            *pp = entry->next;
            lru_remove(entry);
            cache_count--;

            entry->next = doomed;
            doomed = entry;
        }
    }

    return doomed;
}

void pagecache_invalidate(struct vfs_vnode *node)
{
    unsigned long flags = spin_lock_irqsave(&pagecache_lock);
    struct page_cache_entry *doomed = unhook_matching(node->ops, node->internal_info);
    spin_unlock_irqrestore(&pagecache_lock, flags);

    while (doomed) {
        struct page_cache_entry *next = doomed->next;

        if (doomed->dirty && node->ops && node->ops->write_page) {
            node->ops->write_page(node, doomed->page_index, doomed->data, PAGE_SIZE);
        }

        pmm_free_pages(doomed->data);
        slab_free(doomed);
        doomed = next;
    }
}

void pagecache_discard(void *fs_ops, void *file_id)
{
    unsigned long flags = spin_lock_irqsave(&pagecache_lock);
    struct page_cache_entry *doomed = unhook_matching(fs_ops, file_id);
    spin_unlock_irqrestore(&pagecache_lock, flags);

    // No writeback: the file these belong to no longer exists.
    while (doomed) {
        struct page_cache_entry *next = doomed->next;
        pmm_free_pages(doomed->data);
        slab_free(doomed);
        doomed = next;
    }
}
