/*
 * devfs.c - Implementation of the device filesystem (devfs).
 *
 * This file handles the registration and lookup of device nodes within
 * the virtual filesystem, providing a standardized path for device access.
 */

#include "fs/devfs.h"

#include "uapi/errors.h"

#include "fs/vfs.h"
#include "core/tty.h"
#include "mm/slab.h"
#include "mm/heap.h"
#include "core/lock.h"
#include "string.h"
#include "stdio.h"
#include "panic.h"

/*
 * devfs_node - Represents an entry in the devfs directory structure.
 */
struct devfs_node {
    char name[32];
    struct vfs_vnode *vnode;
    struct devfs_node *next;
};

/* Internal filesystem state and synchronization */
static struct vfs_vnode *devfs_root_vnode = NULL;
static struct devfs_node *devfs_devices = NULL;
static spinlock_t devfs_lock = SPINLOCK_INIT;

/* Extern console TTY from the TTY subsystem */
extern struct tty console_tty;

/*
 * devfs_root_lookup - Searches the devfs device list for a node matching
 * the requested filename. Returns the vnode if found, or NULL otherwise.
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
 * devfs_root_readdir - Implementation of the readdir operation for the devfs root.
 */
static int devfs_root_readdir(struct vfs_file *file, void *buffer, size_t count)
{
    struct vfs_dirent *dirent_buf = (struct vfs_dirent *)buffer;
    size_t max_entries = count / sizeof(struct vfs_dirent);
    int entries_read = 0;
    uint32_t current_idx = 0;

    spin_lock(&devfs_lock);
    struct devfs_node *curr = devfs_devices;
    while (curr && entries_read < (int)max_entries) {
        if (current_idx >= (uint32_t)file->offset) {
            struct vfs_dirent *dirent = &dirent_buf[entries_read];
            strncpy(dirent->name, curr->name, 255);
            dirent->name[255] = '\0';
            dirent->ino = 0; // Device nodes don't have inodes in devfs
            file->offset++;
            entries_read++;
        }
        current_idx++;
        curr = curr->next;
    }
    spin_unlock(&devfs_lock);

    return entries_read;
}

/* Operation mapping for the devfs root directory */
static struct vfs_vnode_ops devfs_root_ops = {
    .lookup = devfs_root_lookup,
    .readdir = devfs_root_readdir,
    .read = NULL,
    .write = NULL,
};

/*
 * devfs_register_device - Allocates a vnode and registers a new device
 * in the devfs linked list.
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
 * devfs_get_root - Returns the root directory of the device filesystem.
 */
struct vfs_vnode *devfs_get_root(void)
{
    return devfs_root_vnode;
}

/*
 * devfs_tty_read - Proxies VFS read calls to the TTY driver.
 */
static int devfs_tty_read(struct vfs_file *file, void *buffer, size_t size)
{
    struct tty *tty = (struct tty *)file->node->internal_info;
    return tty_read(tty, (char *)buffer, size);
}

/*
 * devfs_tty_write - Proxies VFS write calls to the TTY driver.
 */
static int devfs_tty_write(struct vfs_file *file, const void *buffer, size_t size)
{
    struct tty *tty = (struct tty *)file->node->internal_info;
    return tty_write(tty, (const char *)buffer, size);
}

/* Operation mapping for TTY device nodes */
static struct vfs_vnode_ops devfs_tty_ops = {
    .read = devfs_tty_read, .write = devfs_tty_write, .lookup = NULL};

/*
 * devfs_init - Boot-time initialization for the device filesystem.
 */
void devfs_init(void)
{
    devfs_root_vnode = (struct vfs_vnode *)slab_alloc(sizeof(struct vfs_vnode));
    if (!devfs_root_vnode) {
        PANIC("Failed to allocate devfs root");
    }

    devfs_root_vnode->type = VFS_VNODE_TYPE_DIR;
    devfs_root_vnode->ops = &devfs_root_ops;
    devfs_root_vnode->internal_info = NULL;
    devfs_root_vnode->parent = NULL;
    devfs_root_vnode->file_size = 0;
    devfs_root_vnode->refcount.counter = 1;

    // Register UART console as the first device in /dev
    if (devfs_register_device("uart", &devfs_tty_ops, &console_tty) != 0) {
        PANIC("Failed to register /dev/uart");
    }

    pr_info("devfs: Device File System initialized\n");
}
