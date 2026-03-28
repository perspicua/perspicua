/*
 * devfs.c - Implementation of the device filesystem (devfs).
 *
 * This module manages the registration and lookup of device nodes within
 * the virtual filesystem, acting as a bridge between VFS calls and drivers.
 */

#include "fs/devfs.h"

#include "stdio.h"
#include "string.h"
#include "panic.h"

#include "uapi/errors.h"

#include "core/tty.h"
#include "core/lock.h"
#include "mm/slab.h"
#include "mm/heap.h"
#include "fs/vfs.h"

/*
 * struct devfs_node - Internal entry in the devfs directory linked-list.
 */
struct devfs_node {
    char name[32];
    struct vfs_vnode *vnode;
    struct devfs_node *next;
};

static struct vfs_vnode *devfs_root_vnode = NULL;
static struct devfs_node *devfs_devices = NULL;
static spinlock_t devfs_lock = SPINLOCK_INIT;

extern struct tty console_tty;

/*
 * devfs_root_lookup - Scans the device list for a matching name.
 */
static struct vfs_vnode *devfs_root_lookup(struct vfs_vnode *dir, const char *filename)
{
    if (dir != devfs_root_vnode) {
        return NULL;
    }

    spin_lock(&devfs_lock);
    struct devfs_node *curr = devfs_devices;
    while (curr) {
        if (strcmp(curr->name, filename) == 0) {
            struct vfs_vnode *node = curr->vnode;
            atomic_inc(&node->refcount);
            spin_unlock(&devfs_lock);
            return node;
        }
        curr = curr->next;
    }
    spin_unlock(&devfs_lock);

    return NULL;
}

/*
 * devfs_root_readdir - Populates a dirent buffer with registered devices.
 */
static int devfs_root_readdir(struct vfs_file *file, void *buffer, size_t count)
{
    struct vfs_dirent *dirent_buf = (struct vfs_dirent *)buffer;
    size_t max_entries = count / sizeof(struct vfs_dirent);
    int entries_read = 0;
    uint32_t current_idx = 0;

    spin_lock(&devfs_lock);
    struct devfs_node *curr = devfs_devices;

    /* Skip entries already read */
    while (curr && current_idx < (uint32_t)file->offset) {
        curr = curr->next;
        current_idx++;
    }

    while (curr && entries_read < (int)max_entries) {
        struct vfs_dirent *dirent = &dirent_buf[entries_read];
        strncpy(dirent->name, curr->name, 255);
        dirent->name[255] = '\0';
        dirent->ino = 0;
        file->offset++;
        entries_read++;
        curr = curr->next;
    }
    spin_unlock(&devfs_lock);

    return entries_read;
}

static int devfs_tty_read(struct vfs_file *file, void *buffer, size_t size)
{
    struct tty *tty = (struct tty *)file->node->internal_info;
    return tty_read(tty, (char *)buffer, size);
}

static int devfs_tty_write(struct vfs_file *file, const void *buffer, size_t size)
{
    struct tty *tty = (struct tty *)file->node->internal_info;
    return tty_write(tty, (const char *)buffer, size);
}

/* Operation tables */
static struct vfs_vnode_ops devfs_root_ops = {.lookup = devfs_root_lookup,
                                              .readdir = devfs_root_readdir};

static struct vfs_vnode_ops devfs_tty_ops = {.read = devfs_tty_read, .write = devfs_tty_write};

/*
 * devfs_register_device - Creates a vnode and links it to the device list.
 */
int devfs_register_device(const char *name, struct vfs_vnode_ops *ops, void *internal_info)
{
    struct vfs_vnode *node = (struct vfs_vnode *)slab_alloc(sizeof(struct vfs_vnode));
    if (!node) {
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    node->type = VFS_VNODE_TYPE_DEVICE;
    node->ops = ops;
    node->internal_info = internal_info;
    node->file_size = 0;
    node->parent = devfs_root_vnode;
    node->refcount.counter = 1;

    struct devfs_node *dev_node = (struct devfs_node *)slab_alloc(sizeof(struct devfs_node));
    if (!dev_node) {
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

/*
 * devfs_get_root - Exposes the root vnode for mounting into the global VFS.
 */
struct vfs_vnode *devfs_get_root(void)
{
    return devfs_root_vnode;
}

/*
 * devfs_init - Initializes the root vnode and registers the primary console.
 */
void devfs_init(void)
{
    devfs_root_vnode = (struct vfs_vnode *)slab_alloc(sizeof(struct vfs_vnode));
    if (!devfs_root_vnode) {
        PANIC("devfs: root allocation failed");
    }

    devfs_root_vnode->type = VFS_VNODE_TYPE_DIR;
    devfs_root_vnode->ops = &devfs_root_ops;
    devfs_root_vnode->internal_info = NULL;
    devfs_root_vnode->parent = NULL;
    devfs_root_vnode->file_size = 0;
    devfs_root_vnode->refcount.counter = 1;

    if (devfs_register_device("uart", &devfs_tty_ops, &console_tty) != 0) {
        PANIC("devfs: console registration failed");
    }

    pr_info("devfs: Device File System initialized\n");
}
