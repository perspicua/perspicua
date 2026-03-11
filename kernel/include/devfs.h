#ifndef _DEVFS_H_
#define _DEVFS_H_

#include "types.h"
#include "vfs.h"

void devfs_init(void);
struct vnode* devfs_get_root(void);
int devfs_register_device(const char* name, struct vnode_ops* ops, void* internal_info);

#endif // _DEVFS_H_
