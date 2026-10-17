/*
 * procfs.h - Public API for the process filesystem (procfs).
 */

#ifndef PERSPICUA_FS_PROCFS_H
#define PERSPICUA_FS_PROCFS_H

#include "fs/vfs.h"

void procfs_init(void);

#ifdef CONFIG_TESTS
    #include <stddef.h>

// Renders the index-th fixed /proc file into buf; -ENOENT past the last one.
int procfs_test_generate(size_t index, char *buf, int size);
#endif

#endif // PERSPICUA_FS_PROCFS_H
