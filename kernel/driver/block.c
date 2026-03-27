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

/* --- Private Macros --- */

#define BLOCK_MAX_DEVICES 8

/* --- Private Variables --- */

static struct block_device *devices[BLOCK_MAX_DEVICES];
static size_t nr_devices = 0;

/* --- Private Helper Functions --- */

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

/* --- Public API Implementations --- */

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

    devices[nr_devices++] = dev;

    if (devfs_register_device(dev->name, &block_device_vfs_ops, dev) != PERS_SUCCESS) {
        pr_err("block: failed to register /dev/%s\n", dev->name);
    }
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
