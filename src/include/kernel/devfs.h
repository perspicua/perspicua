#ifndef _DEVFS_H_
#define _DEVFS_H_

#include "lib/types.h"
#include "kernel/vfs.h"

void devfs_init(void);

int devfs_uart_write(struct file* file, const void* buffer, size_t size);
int devfs_uart_read(struct file* file, void* buffer, size_t size);

struct vnode* devfs_root_lookup(struct vnode* dir, const char* filename);
struct vnode* devfs_dev_lookup(struct vnode* dir, const char* filename);

#endif // _DEVFS_H_
