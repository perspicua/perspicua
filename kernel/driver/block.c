/*
 * block.c - Generic block device management layer.
 *
 * This layer provides a unified interface for block-oriented hardware
 * (like SD cards, disks, etc.) and integrates them into the VFS via devfs.
 */

#include "driver/block.h"

#include "uapi/errors.h"
#include "stdio.h"
#include "string.h"

#include "fs/vfs.h"
#include "fs/devfs.h"
#include "mm/slab.h"

#define BLOCK_MAX_DEVICES 8

static struct block_device* devices[BLOCK_MAX_DEVICES];
static size_t nr_devices = 0;

/*
 * block_device_vfs_read - VFS bridge for block device reads.
 */
static int block_device_vfs_read(struct vfs_file* file, void* buffer, size_t size)
{
    struct block_device* dev = (struct block_device*)file->node->internal_info;

    if (file->offset % dev->block_size != 0 || size % dev->block_size != 0)
    {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    size_t start_block = file->offset / dev->block_size;
    size_t num_blocks = size / dev->block_size;

    int res = dev->read_blocks(dev, buffer, start_block, num_blocks);
    if (res == PERS_SUCCESS)
    {
        file->offset += size;
        return (int)size;
    }

    return res;
}

/*
 * block_device_vfs_write - VFS bridge for block device writes.
 */
static int block_device_vfs_write(struct vfs_file* file, const void* buffer, size_t size)
{
    struct block_device* dev = (struct block_device*)file->node->internal_info;

    if (file->offset % dev->block_size != 0 || size % dev->block_size != 0)
    {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    size_t start_block = file->offset / dev->block_size;
    size_t num_blocks = size / dev->block_size;

    int res = dev->write_blocks(dev, buffer, start_block, num_blocks);
    if (res == PERS_SUCCESS)
    {
        file->offset += size;
        return (int)size;
    }

    return res;
}

/* Operation mapping for block devices exposed in devfs */
static struct vfs_vnode_ops block_device_vfs_ops = {
    .read = block_device_vfs_read, .write = block_device_vfs_write, .lookup = NULL, .close = NULL};

/*
 * block_device_register - Adds a new block device to the system.
 */
void block_device_register(struct block_device* dev)
{
    if (!dev)
    {
        return;
    }

    if (nr_devices >= BLOCK_MAX_DEVICES)
    {
        printf("[ BLOCK ] Registration failed: device table full\n");
        return;
    }

    devices[nr_devices++] = dev;

    if (devfs_register_device(dev->name, &block_device_vfs_ops, dev) != PERS_SUCCESS)
    {
        printf("[ BLOCK ] Failed to register /dev/%s\n", dev->name);
    }
}

/*
 * block_device_lookup - Finds a registered block device by name.
 */
struct block_device* block_device_lookup(const char* name)
{
    if (!name)
    {
        return NULL;
    }

    for (size_t i = 0; i < nr_devices; i++)
    {
        if (devices[i] && strcmp(name, devices[i]->name) == 0)
        {
            return devices[i];
        }
    }

    return NULL;
}
