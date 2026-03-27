/*
 * block.h - Public API for the generic block device layer.
 *
 * This header defines the interface for managing block-oriented storage
 * devices and integrating them into the system's VFS.
 */

#ifndef PERSPICUA_DRIVER_BLOCK_H
#define PERSPICUA_DRIVER_BLOCK_H

#include "types.h"

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

/*
 * block_device_register - Adds a new block device to the system registry.
 */
void block_device_register(struct block_device *dev);

/*
 * block_device_lookup - Locates a block device by its name.
 */
struct block_device *block_device_lookup(const char *name);

#endif /* PERSPICUA_DRIVER_BLOCK_H */
