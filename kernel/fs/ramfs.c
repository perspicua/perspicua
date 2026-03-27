/*
 * ramfs.c - Implementation of the RAM-based filesystem (ramfs).
 *
 * This module implements a simple read-only in-memory filesystem used
 * for the initial system root.
 */

#include "fs/ramfs.h"

#include "stdio.h"
#include "string.h"

#include "uapi/errors.h"

#include "core/lock.h"
#include "mm/slab.h"
#include "mm/heap.h"
#include "fs/vfs.h"

#define RAMFS_MAX_FILES 32

/*
 * struct ramfs_file_data - Mapping between file metadata and its memory buffer.
 */
struct ramfs_file_data {
    const char *name;
    const void *data;
    size_t size;
    struct vfs_vnode *node;
};

static struct ramfs_file_data ramfs_files[RAMFS_MAX_FILES];
static int ramfs_file_count = 0;

static struct vfs_vnode *ramfs_root_vnode = NULL;
static struct vfs_vnode_ops ramfs_dir_ops;
static struct vfs_vnode_ops ramfs_file_ops;

static int ramfs_readdir(struct vfs_file *file, void *buffer, size_t count)
{
    struct vfs_dirent *dirent_buf = (struct vfs_dirent *)buffer;
    size_t max_entries = count / sizeof(struct vfs_dirent);
    int entries_read = 0;

    for (int i = (int)file->offset; i < ramfs_file_count && entries_read < (int)max_entries; i++) {
        struct vfs_dirent *dirent = &dirent_buf[entries_read];
        strncpy(dirent->name, ramfs_files[i].name, 255);
        dirent->name[255] = '\0';
        dirent->ino = 0;
        file->offset++;
        entries_read++;
    }

    return entries_read;
}

/*
 * ramfs_read - Direct memory-to-buffer copy for file contents.
 */
int ramfs_read(struct vfs_file *file, void *buffer, size_t size)
{
    struct ramfs_file_data *data = (struct ramfs_file_data *)file->node->internal_info;
    if (!data) {
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    if (file->offset >= (vfs_off_t)data->size) {
        return 0;
    }

    size_t bytes_to_read = size;
    if (file->offset + (vfs_off_t)bytes_to_read > (vfs_off_t)data->size) {
        bytes_to_read = data->size - (size_t)file->offset;
    }

    memcpy(buffer, (const char *)data->data + file->offset, bytes_to_read);

    return (int)bytes_to_read;
}

/*
 * ramfs_lookup - Scans the static file registry for a name match.
 */
struct vfs_vnode *ramfs_lookup(struct vfs_vnode *dir, const char *filename)
{
    if (dir->type != VFS_VNODE_TYPE_DIR) {
        return NULL;
    }

    for (int i = 0; i < ramfs_file_count; i++) {
        if (strcmp(filename, ramfs_files[i].name) == 0) {
            atomic_inc(&ramfs_files[i].node->refcount);
            return ramfs_files[i].node;
        }
    }

    return NULL;
}

/*
 * ramfs_register_file - Allocates a vnode and binds it to a memory region.
 */
void ramfs_register_file(const char *name, const void *data, size_t size)
{
    if (ramfs_file_count >= RAMFS_MAX_FILES) {
        return;
    }

    ramfs_files[ramfs_file_count].name = name;
    ramfs_files[ramfs_file_count].data = data;
    ramfs_files[ramfs_file_count].size = size;

    struct vfs_vnode *vn = (struct vfs_vnode *)slab_alloc(sizeof(struct vfs_vnode));
    if (!vn) {
        return;
    }

    vn->type = VFS_VNODE_TYPE_REGULAR;
    vn->ops = &ramfs_file_ops;
    vn->file_size = (vfs_off_t)size;
    vn->parent = ramfs_root_vnode;
    vn->internal_info = &ramfs_files[ramfs_file_count];
    vn->refcount.counter = 1;

    ramfs_files[ramfs_file_count].node = vn;
    ramfs_file_count++;

    pr_info("ramfs: registered file: %s (%u bytes)\n", name, (unsigned int)size);
}

/*
 * ramfs_init - Initializes operation tables and mounts the root directory.
 */
void ramfs_init(void)
{
    ramfs_root_vnode = (struct vfs_vnode *)slab_alloc(sizeof(struct vfs_vnode));
    if (!ramfs_root_vnode) {
        return;
    }

    ramfs_dir_ops.lookup = ramfs_lookup;
    ramfs_dir_ops.readdir = ramfs_readdir;

    ramfs_file_ops.read = ramfs_read;

    ramfs_root_vnode->type = VFS_VNODE_TYPE_DIR;
    ramfs_root_vnode->ops = &ramfs_dir_ops;
    ramfs_root_vnode->internal_info = NULL;
    ramfs_root_vnode->parent = NULL;
    ramfs_root_vnode->file_size = 0;
    ramfs_root_vnode->refcount.counter = 1;

    vfs_mount("/", ramfs_root_vnode);

    static const char *hello = "Hello from the Raspberry Pi 4 RAMFS!\n";
    ramfs_register_file("hello.txt", hello, strlen(hello));
}
