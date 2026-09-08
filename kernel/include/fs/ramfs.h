/*
 * ramfs.h - Public API for the RAM-based filesystem (ramfs).
 */

#ifndef PERSPICUA_FS_RAMFS_H
#define PERSPICUA_FS_RAMFS_H

#include "types.h"

#include "fs/vfs.h"

void ramfs_init(void);

void ramfs_register_file(const char *name, const void *data, size_t size);

int ramfs_read(struct vfs_file *file, void *buffer, size_t size);

struct vfs_vnode *ramfs_lookup(struct vfs_vnode *dir, const char *filename);

#endif // PERSPICUA_FS_RAMFS_H
