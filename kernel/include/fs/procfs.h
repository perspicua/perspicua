/*
 * procfs.h - Public API for the process filesystem (procfs).
 *
 * This header defines the initialization interface for the virtual
 * filesystem that exposes kernel and process metadata.
 */

#ifndef PERSPICUA_FS_PROCFS_H
#define PERSPICUA_FS_PROCFS_H

#include "fs/vfs.h"

/* --- Function Prototypes --- */

/*
 * procfs_init - Initializes the proc filesystem and mounts it at /proc.
 */
void procfs_init(void);

#endif /* PERSPICUA_FS_PROCFS_H */
