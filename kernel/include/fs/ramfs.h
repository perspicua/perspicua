/*
 * ramfs.h - Public API for the RAM-based filesystem (ramfs).
 *
 * This header defines the interface for initializing the RAM filesystem
 * and registering static files within it.
 */

#ifndef PERSPICUA_FS_RAMFS_H
#define PERSPICUA_FS_RAMFS_H

#include "types.h"

#include "fs/vfs.h"

/* --- Function Prototypes --- */

/*
 * ramfs_init - Boot-time setup for the RAM filesystem.
 */
void ramfs_init(void);

/*
 * ramfs_register_file - Injects a static buffer into the RAMFS file registry.
 */
void ramfs_register_file(const char *name, const void *data, size_t size);

/*
 * ramfs_read - VFS bridge for reading RAMFS file contents.
 */
int ramfs_read(struct vfs_file *file, void *buffer, size_t size);

/*
 * ramfs_lookup - VFS bridge for finding files within RAMFS.
 */
struct vfs_vnode *ramfs_lookup(struct vfs_vnode *dir, const char *filename);

#endif /* PERSPICUA_FS_RAMFS_H */
