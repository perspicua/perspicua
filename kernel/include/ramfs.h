/*
 * ramfs.h - Public API for the RAM-based filesystem (ramfs).
 *
 * This file defines the interface for initializing the RAM filesystem
 * and registering static files within it.
 */

#ifndef PERSPICUA_KERNEL_RAMFS_H
#define PERSPICUA_KERNEL_RAMFS_H

#include "types.h"
#include "vfs.h"

/*
 * ramfs_init - Initializes the RAM filesystem, creating its root vnode
 * and mounting it to the system root directory.
 */
void ramfs_init(void);

/*
 * ramfs_register_file - Adds a static file to the RAM filesystem.
 * This is primarily used by the InitRD loader to populate the root filesystem.
 */
void ramfs_register_file(const char* name, const void* data, size_t size);

/*
 * ramfs_read - VFS operation to read data from a file in the RAM filesystem.
 */
int ramfs_read(struct file* file, void* buffer, size_t size);

/*
 * ramfs_lookup - VFS operation to find a vnode by name within a RAMFS directory.
 */
struct vnode* ramfs_lookup(struct vnode* dir, const char* filename);

#endif /* PERSPICUA_KERNEL_RAMFS_H */
