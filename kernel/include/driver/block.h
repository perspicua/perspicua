/*
 * block.h - Public API for the generic block device layer.
 *
 * This file defines the common structure and registration interface for
 * all block-oriented storage devices in the system.
 */

#ifndef PERSPICUA_DRIVER_BLOCK_H
#define PERSPICUA_DRIVER_BLOCK_H

#include "types.h"

/*
 * block_device - Represents a generic storage device.
 */
struct block_device {
    char name[64];
    size_t block_count;
    size_t block_size;

    /*
     * read_blocks: driver-specific read implementation.
     * write_blocks: driver-specific write implementation.
     */
    int (*read_blocks)(struct block_device *dev, void *buffer, size_t start_block,
                       size_t num_blocks);
    int (*write_blocks)(struct block_device *dev, const void *buffer, size_t start_block,
                        size_t num_blocks);

    int present;
    void *private_data;
};

/*
 * block_device_register - Adds a new block device to the system.
 */
void block_device_register(struct block_device *dev);

/*
 * block_device_lookup - Finds a registered block device by name.
 */
struct block_device *block_device_lookup(const char *name);

#endif /* PERSPICUA_DRIVER_BLOCK_H */
