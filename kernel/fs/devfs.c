#include "devfs.h"
#include "heap.h"
#include "uapi/errors.h"
#include "vfs.h"
#include "tty.h"
#include "slab.h"
#include "lock.h"
#include "string.h"
#include "panic.h"

struct devfs_node
{
    char name[32];
    struct vnode* vnode;
    struct devfs_node* next;
};

static struct vnode* devfs_root_vnode = NULL;
static struct devfs_node* devfs_devices = NULL;
static spinlock_t devfs_lock = SPINLOCK_INIT;

static struct vnode* devfs_root_lookup(struct vnode* dir, const char* filename)
{
    if (dir != devfs_root_vnode)
        return NULL;

    spin_lock(&devfs_lock);
    struct devfs_node* curr = devfs_devices;
    while (curr)
    {
        if (strcmp(curr->name, filename) == 0)
        {
            struct vnode* node = curr->vnode;
            atomic_inc(&node->refcount);
            spin_unlock(&devfs_lock);
            return node;
        }
        curr = curr->next;
    }
    spin_unlock(&devfs_lock);

    return NULL;
}

static struct vnode_ops devfs_root_ops = {.lookup = devfs_root_lookup, .read = NULL, .write = NULL};

int devfs_register_device(const char* name, struct vnode_ops* ops, void* internal_info)
{
    struct vnode* node = (struct vnode*)slab_alloc(sizeof(struct vnode));
    if (!node)
        return -PERS_ERR_OUT_OF_MEMORY;

    node->type = VNODE_TYPE_DEVICE;
    node->ops = ops;
    node->internal_info = internal_info;
    node->filesize = 0;
    node->parent = devfs_root_vnode;
    node->refcount.counter = 1;

    struct devfs_node* dev_node = (struct devfs_node*)slab_alloc(sizeof(struct devfs_node));
    if (!dev_node)
    {
        slab_free(node);
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    strncpy(dev_node->name, name, 31);
    dev_node->name[31] = '\0';
    dev_node->vnode = node;

    spin_lock(&devfs_lock);
    dev_node->next = devfs_devices;
    devfs_devices = dev_node;
    spin_unlock(&devfs_lock);

    return PERS_SUCCESS;
}

struct vnode* devfs_get_root(void)
{
    return devfs_root_vnode;
}

extern struct tty console_tty;

static int devfs_tty_read(struct file* file, void* buffer, size_t size)
{
    struct tty* tty = (struct tty*)file->node->internal_info;
    return tty_read(tty, (char*)buffer, size);
}

static int devfs_tty_write(struct file* file, const void* buffer, size_t size)
{
    struct tty* tty = (struct tty*)file->node->internal_info;
    return tty_write(tty, (const char*)buffer, size);
}

static struct vnode_ops devfs_tty_ops = {.read = devfs_tty_read, .write = devfs_tty_write, .lookup = NULL};

void devfs_init(void)
{
    devfs_root_vnode = (struct vnode*)slab_alloc(sizeof(struct vnode));
    if (!devfs_root_vnode)
        PANIC("Failed to allocate devfs root");

    devfs_root_vnode->type = VNODE_TYPE_DIR;
    devfs_root_vnode->ops = &devfs_root_ops;
    devfs_root_vnode->internal_info = NULL;
    devfs_root_vnode->parent = NULL;
    devfs_root_vnode->filesize = 0;
    devfs_root_vnode->refcount.counter = 1;

    if (devfs_register_device("uart", &devfs_tty_ops, &console_tty) != 0)
        PANIC("Failed to register /dev/uart");
}
