/*
 * devfs.h - Public API for the device filesystem (devfs).
 */

#ifndef PERSPICUA_FS_DEVFS_H
#define PERSPICUA_FS_DEVFS_H

#include "types.h"

#include "fs/vfs.h"

void devfs_init(void);

struct vfs_vnode *devfs_get_root(void);

int devfs_register_device(const char *name, struct vfs_vnode_ops *ops, void *internal_info);

extern struct vfs_vnode_ops devfs_tty_ops;

#endif // PERSPICUA_FS_DEVFS_H
