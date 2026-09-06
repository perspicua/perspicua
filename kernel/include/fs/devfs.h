/*
 * devfs.h - Public API for the device filesystem (devfs).
 *
 * This header defines the interface for initializing the device filesystem
 * and registering device nodes for access through the VFS.
 */

#ifndef PERSPICUA_FS_DEVFS_H
#define PERSPICUA_FS_DEVFS_H

#include "types.h"

#include "fs/vfs.h"

/*
 * devfs_init - Boot-time setup for the device filesystem.
 */
void devfs_init(void);

/*
 * devfs_get_root - Returns the root vnode of the device filesystem.
 */
struct vfs_vnode *devfs_get_root(void);

/*
 * devfs_register_device - Mounts a new device into the devfs structure.
 */
int devfs_register_device(const char *name, struct vfs_vnode_ops *ops, void *internal_info);

extern struct vfs_vnode_ops devfs_tty_ops;

#endif /* PERSPICUA_FS_DEVFS_H */
