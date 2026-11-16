/*
 * block.h - Public API for the generic block device layer.
 */

#ifndef PERSPICUA_DRIVER_BLOCK_H
#define PERSPICUA_DRIVER_BLOCK_H

#include <stddef.h>

/*
 * struct block_device - Represents a generic storage unit.
 *
 * Provides a unified abstraction for hardware-specific block operations
 * (read/write).
 */
struct block_device {
    char name[64];
    size_t block_count;
    size_t block_size;

    int (*read_blocks)(struct block_device *dev, void *buffer, size_t start_block,
                       size_t num_blocks);
    int (*write_blocks)(struct block_device *dev, const void *buffer, size_t start_block,
                        size_t num_blocks);

    int present;
    void *private_data;
};

void block_device_register(struct block_device *dev);

struct block_device *block_device_lookup(const char *name);

int block_cache_sync(void);

#ifdef CONFIG_TESTS
// Drops every clean cached block, so the next read of it reaches the device.
void block_cache_invalidate(void);

// Called by a cached read between its device read and its cache fill; NULL when unset.
extern void (*block_test_before_fill)(struct block_device *dev, size_t start, size_t count);
#endif

#endif // PERSPICUA_DRIVER_BLOCK_H
