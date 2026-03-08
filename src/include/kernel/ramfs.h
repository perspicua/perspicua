#ifndef _RAMFS_H_
#define _RAMFS_H_

#include "lib/types.h"
#include "kernel/vfs.h"

void ramfs_init(void);

int ramfs_read(struct file* file, void* buffer, size_t size);

struct vnode* ramfs_lookup(struct vnode* dir, const char* filename);

#endif // _RAMFS_H_
