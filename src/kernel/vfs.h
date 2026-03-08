#ifndef _VFS_H_
#define _VFS_H_

#include "../lib/types.h"
struct vnode;
struct file;

typedef enum
{
    VNODE_TYPE_REGULAR,
    VNODE_TYPE_DIR,
    VNODE_TYPE_DEVICE
} vnode_type_t;

struct vnode
{
    vnode_type_t type;
    size_t filesize;
    struct vnode_ops* ops;
    void* internal_info;
};

struct vnode_ops
{
    int (*read)(struct file* file, void* buffer, size_t size);
    int (*write)(struct file* file, const void* buffer, size_t size);
    struct vnode* (*lookup)(struct vnode* dir, const char* filename);
};

struct file
{
    struct vnode* node;
    uint32_t offset;
    int flags;
};
#endif // _VFS_H_
