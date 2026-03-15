/*
 * devfs.h - Public API for the device filesystem (devfs).
 *
 * This file defines the interface for initializing the device filesystem
 * and registering device nodes that can be accessed through the VFS.
 */

#ifndef PERSPICUA_KERNEL_DEVFS_H
#define PERSPICUA_KERNEL_DEVFS_H

#include "types.h"
#include "vfs.h"

/*
 * devfs_init - Initializes the device filesystem, creating the root vnode
 * and registering initial system devices like the UART.
 */
void devfs_init(void);

/*
 * devfs_get_root - Returns the root vnode of the device filesystem.
 */
struct vfs_vnode* devfs_get_root(void);

/*
 * devfs_register_device - Registers a new device node in devfs with the
 * given name and vnode operations. Returns PERS_SUCCESS on success or
 * a negative error code on failure.
 */
int devfs_register_device(const char* name, struct vfs_vnode_ops* ops, void* internal_info);

#endif /* PERSPICUA_KERNEL_DEVFS_H */
