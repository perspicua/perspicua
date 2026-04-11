/*
 * block.c - Generic block device management layer.
 *
 * This module provides a unified interface for block-oriented hardware
 * and handles their registration into the device filesystem (devfs).
 */

#include "driver/block.h"

#include "stdio.h"
#include "string.h"

#include "uapi/errors.h"

#include "fs/vfs.h"
#include "fs/devfs.h"
#include "mm/slab.h"
#include "mm/pmm.h"
#include "core/lock.h"

#define BLOCK_MAX_DEVICES 8
#define BLOCK_CACHE_SIZE  256
#define BLOCK_HASH_SIZE   64

struct block_cache_entry {
    struct block_device *dev;
    size_t block_nr;
    void *data;
    int dirty;
    struct block_cache_entry *next; // Hash chain
    struct block_cache_entry *lru_next;
    struct block_cache_entry *lru_prev;
};

static struct block_cache_entry *hash_table[BLOCK_HASH_SIZE];
static struct block_cache_entry *lru_head = NULL;
static struct block_cache_entry *lru_tail = NULL;
static size_t cache_count = 0;
static spinlock_t cache_lock = SPINLOCK_INIT;

static size_t block_hash(struct block_device *dev, size_t block_nr)
{
    return ((uintptr_t)dev + block_nr) % BLOCK_HASH_SIZE;
}

static void lru_remove(struct block_cache_entry *entry)
{
    if (entry->lru_prev)
        entry->lru_prev->lru_next = entry->lru_next;
    else
        lru_head = entry->lru_next;

    if (entry->lru_next)
        entry->lru_next->lru_prev = entry->lru_prev;
    else
        lru_tail = entry->lru_prev;

    entry->lru_next = entry->lru_prev = NULL;
}

static void lru_add_head(struct block_cache_entry *entry)
{
    entry->lru_next = lru_head;
    entry->lru_prev = NULL;
    if (lru_head)
        lru_head->lru_prev = entry;
    lru_head = entry;
    if (!lru_tail)
        lru_tail = entry;
}

static struct block_cache_entry *cache_lookup(struct block_device *dev, size_t block_nr)
{
    size_t h = block_hash(dev, block_nr);
    struct block_cache_entry *curr = hash_table[h];
    while (curr) {
        if (curr->dev == dev && curr->block_nr == block_nr) {
            lru_remove(curr);
            lru_add_head(curr);
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

/* Forward declarations for the original driver functions we wrap */
struct block_ops_wrapper {
    int (*orig_read_blocks)(struct block_device *dev, void *buffer, size_t start_block,
                            size_t num_blocks);
    int (*orig_write_blocks)(struct block_device *dev, const void *buffer, size_t start_block,
                             size_t num_blocks);
};

static int cached_read_blocks(struct block_device *dev, void *buffer, size_t start_block,
                              size_t num_blocks)
{
    struct block_ops_wrapper *ops = (struct block_ops_wrapper *)dev->private_data;
    unsigned long flags = spin_lock_irqsave(&cache_lock);

    for (size_t i = 0; i < num_blocks; i++) {
        size_t block_nr = start_block + i;
        struct block_cache_entry *entry = cache_lookup(dev, block_nr);

        if (entry) {
            memcpy((uint8_t *)buffer + i * dev->block_size, entry->data, dev->block_size);
            continue;
        }

        /* Miss: Must read from disk - release lock first to avoid holding it during I/O */
        spin_unlock_irqrestore(&cache_lock, flags);

        void *temp_buf = pmm_alloc_pages((dev->block_size + PAGE_SIZE - 1) / PAGE_SIZE);
        if (!temp_buf) {
            return -PERS_ERR_OUT_OF_MEMORY;
        }

        int res = ops->orig_read_blocks(dev, temp_buf, block_nr, 1);
        if (res != PERS_SUCCESS) {
            pmm_free_pages(temp_buf, (dev->block_size + PAGE_SIZE - 1) / PAGE_SIZE);
            return res;
        }

        /* Re-acquire lock to update cache */
        flags = spin_lock_irqsave(&cache_lock);

        /* Check if another thread cached it while we were reading */
        entry = cache_lookup(dev, block_nr);
        if (entry) {
            memcpy((uint8_t *)buffer + i * dev->block_size, entry->data, dev->block_size);
            pmm_free_pages(temp_buf, (dev->block_size + PAGE_SIZE - 1) / PAGE_SIZE);
            continue;
        }

        /* Evict if needed */
        size_t pages_needed = (dev->block_size + PAGE_SIZE - 1) / PAGE_SIZE;
        if (cache_count >= BLOCK_CACHE_SIZE) {
            struct block_cache_entry *evict = lru_tail;

            /* Flush dirty block to disk before eviction */
            if (evict->dirty) {
                /* Must release lock during I/O */
                void *evict_data = evict->data;
                size_t evict_block_nr = evict->block_nr;
                struct block_device *evict_dev = evict->dev;
                spin_unlock_irqrestore(&cache_lock, flags);

                struct block_ops_wrapper *evict_ops =
                    (struct block_ops_wrapper *)evict_dev->private_data;
                evict_ops->orig_write_blocks(evict_dev, evict_data, evict_block_nr, 1);

                flags = spin_lock_irqsave(&cache_lock);
                /* Re-lookup evict entry as it might have changed */
                evict = lru_tail;
                if (!evict) {
                    pmm_free_pages(temp_buf, pages_needed);
                    spin_unlock_irqrestore(&cache_lock, flags);
                    return -PERS_ERR_UNKNOWN;
                }
            }

            lru_remove(evict);
            size_t eh = block_hash(evict->dev, evict->block_nr);
            struct block_cache_entry **pp = &hash_table[eh];
            while (*pp && *pp != evict)
                pp = &((*pp)->next);
            if (*pp == evict)
                *pp = evict->next;

            /* If the evicted entry's buffer size doesn't match, reallocate */
            size_t old_pages = (evict->dev->block_size + PAGE_SIZE - 1) / PAGE_SIZE;
            if (old_pages != pages_needed) {
                pmm_free_pages(evict->data, old_pages);
                evict->data = temp_buf;
            } else {
                memcpy(evict->data, temp_buf, dev->block_size);
                pmm_free_pages(temp_buf, pages_needed);
            }
            entry = evict;
        } else {
            entry = slab_alloc(sizeof(struct block_cache_entry));
            entry->data = temp_buf;
            cache_count++;
        }

        if (!entry->data) {
            spin_unlock_irqrestore(&cache_lock, flags);
            return -PERS_ERR_OUT_OF_MEMORY;
        }

        entry->dev = dev;
        entry->block_nr = block_nr;
        entry->dirty = 0;
        if (entry->data != temp_buf) {
            /* Data was copied to existing buffer */
        }

        size_t h = block_hash(dev, block_nr);
        entry->next = hash_table[h];
        hash_table[h] = entry;
        lru_add_head(entry);

        memcpy((uint8_t *)buffer + i * dev->block_size, entry->data, dev->block_size);
    }

    spin_unlock_irqrestore(&cache_lock, flags);
    return PERS_SUCCESS;
}

static int cached_write_blocks(struct block_device *dev, const void *buffer, size_t start_block,
                               size_t num_blocks)
{
    struct block_ops_wrapper *ops = (struct block_ops_wrapper *)dev->private_data;

    /* Write-through strategy for simplicity */
    int res = ops->orig_write_blocks(dev, buffer, start_block, num_blocks);
    if (res != PERS_SUCCESS)
        return res;

    unsigned long flags = spin_lock_irqsave(&cache_lock);
    for (size_t i = 0; i < num_blocks; i++) {
        size_t block_nr = start_block + i;
        struct block_cache_entry *entry = cache_lookup(dev, block_nr);
        if (entry) {
            memcpy(entry->data, (const uint8_t *)buffer + i * dev->block_size, dev->block_size);
        }
    }
    spin_unlock_irqrestore(&cache_lock, flags);

    return PERS_SUCCESS;
}

/*
 * block_cache_sync - Flushes all dirty cache entries to their backing devices.
 *
 * Releases the cache lock during I/O to avoid sleeping with a spinlock held
 * (the SD driver's sd_op_acquire() can block).
 */
int block_cache_sync(void)
{
    int flushed = 0;
    int made_progress;

    do {
        made_progress = 0;
        unsigned long flags = spin_lock_irqsave(&cache_lock);

        struct block_cache_entry *entry = lru_head;
        while (entry) {
            if (entry->dirty) {
                /* Snapshot fields before releasing lock. */
                struct block_device *dev = entry->dev;
                size_t block_nr = entry->block_nr;
                void *data = entry->data;
                struct block_ops_wrapper *ops = (struct block_ops_wrapper *)dev->private_data;

                spin_unlock_irqrestore(&cache_lock, flags);

                int res = ops->orig_write_blocks(dev, data, block_nr, 1);

                flags = spin_lock_irqsave(&cache_lock);

                /* Re-lookup: the entry may have been evicted while unlocked. */
                size_t h = block_hash(dev, block_nr);
                struct block_cache_entry *cur = hash_table[h];
                while (cur) {
                    if (cur->dev == dev && cur->block_nr == block_nr)
                        break;
                    cur = cur->next;
                }
                if (cur && res == PERS_SUCCESS) {
                    cur->dirty = 0;
                    flushed++;
                    made_progress = 1;
                }

                /* Restart scan from the head since list may have changed. */
                break;
            }
            entry = entry->lru_next;
        }

        spin_unlock_irqrestore(&cache_lock, flags);
    } while (made_progress);

    return flushed;
}

static struct block_device *devices[BLOCK_MAX_DEVICES];
static size_t nr_devices = 0;

/*
 * block_device_vfs_read - Bridge between VFS byte-reads and driver block-reads.
 */
static int block_device_vfs_read(struct vfs_file *file, void *buffer, size_t size)
{
    struct block_device *dev = (struct block_device *)file->node->internal_info;

    /* Enforce block-aligned offsets and sizes */
    if (file->offset % dev->block_size != 0 || size % dev->block_size != 0) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    size_t start_block = file->offset / dev->block_size;
    size_t num_blocks = size / dev->block_size;

    int res = dev->read_blocks(dev, buffer, start_block, num_blocks);
    if (res == PERS_SUCCESS) {
        file->offset += size;
        return (int)size;
    }

    return res;
}

/*
 * block_device_vfs_write - Bridge between VFS byte-writes and driver block-writes.
 */
static int block_device_vfs_write(struct vfs_file *file, const void *buffer, size_t size)
{
    struct block_device *dev = (struct block_device *)file->node->internal_info;

    if (file->offset % dev->block_size != 0 || size % dev->block_size != 0) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    size_t start_block = file->offset / dev->block_size;
    size_t num_blocks = size / dev->block_size;

    int res = dev->write_blocks(dev, buffer, start_block, num_blocks);
    if (res == PERS_SUCCESS) {
        file->offset += size;
        return (int)size;
    }

    return res;
}

/* Operation mapping for devfs registration */
static struct vfs_vnode_ops block_device_vfs_ops = {
    .read = block_device_vfs_read, .write = block_device_vfs_write, .lookup = NULL, .close = NULL};

/*
 * block_device_register - Adds a device to the global table and registers it in devfs.
 */
void block_device_register(struct block_device *dev)
{
    if (!dev) {
        return;
    }

    if (nr_devices >= BLOCK_MAX_DEVICES) {
        pr_err("block: registration failed: table full\n");
        return;
    }

    /* Wrap operations with cache */
    struct block_ops_wrapper *wrapper = slab_alloc(sizeof(struct block_ops_wrapper));
    wrapper->orig_read_blocks = dev->read_blocks;
    wrapper->orig_write_blocks = dev->write_blocks;

    dev->private_data = wrapper;
    dev->read_blocks = cached_read_blocks;
    dev->write_blocks = cached_write_blocks;

    devices[nr_devices++] = dev;

    if (devfs_register_device(dev->name, &block_device_vfs_ops, dev) != PERS_SUCCESS) {
        pr_err("block: failed to register /dev/%s\n", dev->name);
    }

    pr_info("block: registered /dev/%s with LRU cache (%d entries)\n", dev->name, BLOCK_CACHE_SIZE);
}

/*
 * block_device_lookup - Returns a device pointer matching the provided name.
 */
struct block_device *block_device_lookup(const char *name)
{
    if (!name) {
        return NULL;
    }

    for (size_t i = 0; i < nr_devices; i++) {
        if (devices[i] && strcmp(name, devices[i]->name) == 0) {
            return devices[i];
        }
    }

    return NULL;
}
