#ifndef PERSPICUA_KERNEL_PROCFS_H
#define PERSPICUA_KERNEL_PROCFS_H

#include "fs/vfs.h"

/*
 * procfs_init - Initializes the proc filesystem and registers it with VFS.
 */
void procfs_init(void);

#endif /* PERSPICUA_KERNEL_PROCFS_H */
