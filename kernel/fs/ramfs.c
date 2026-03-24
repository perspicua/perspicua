/*
 * ramfs.c - Implementation of the RAM-based filesystem (ramfs).
 *
 * This file implements a simple, read-only in-memory filesystem used for
 * the initial system root and static system files.
 */

#include "fs/ramfs.h"

#include "uapi/errors.h"

#include "fs/vfs.h"
#include "mm/slab.h"
#include "mm/heap.h"
#include "core/lock.h"
#include "string.h"
#include "stdio.h"

/*
 * ramfs_file_data - Internal representation of a file in the RAM filesystem.
 */
struct ramfs_file_data
{
    const char* name;
    const void* data;
    size_t size;
    struct vfs_vnode* node;
};

/* Internal filesystem state and maximum file limit */
#define RAMFS_MAX_FILES 32
static struct ramfs_file_data ramfs_files[RAMFS_MAX_FILES];
static int ramfs_file_count = 0;

/* VFS nodes and operation tables for RAMFS */
static struct vfs_vnode* ramfs_root_vnode = NULL;
static struct vfs_vnode_ops ramfs_dir_ops;
static struct vfs_vnode_ops ramfs_file_ops;

/*
 * ramfs_read - Reads data from a RAMFS file into the provided buffer.
 */
int ramfs_read(struct vfs_file* file, void* buffer, size_t size)
{
    struct ramfs_file_data* data = (struct ramfs_file_data*)file->node->internal_info;
    if (!data)
    {
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    if (file->offset >= (vfs_off_t)data->size)
    {
        return 0;  // End of file reached
    }

    size_t bytes_to_read = size;
    if (file->offset + (vfs_off_t)bytes_to_read > (vfs_off_t)data->size)
    {
        bytes_to_read = data->size - (size_t)file->offset;
    }

    memcpy(buffer, (const char*)data->data + file->offset, bytes_to_read);

    return (int)bytes_to_read;
}

/*
 * ramfs_readdir - Reads directory entries from the RAMFS root.
 */
static int ramfs_readdir(struct vfs_file* file, void* buffer, size_t count)
{
    struct vfs_dirent* dirent_buf = (struct vfs_dirent*)buffer;
    size_t max_entries = count / sizeof(struct vfs_dirent);
    int entries_read = 0;

    for (int i = (int)file->offset; i < ramfs_file_count && entries_read < (int)max_entries; i++)
    {
        struct vfs_dirent* dirent = &dirent_buf[entries_read];
        strncpy(dirent->name, ramfs_files[i].name, 255);
        dirent->name[255] = '\0';
        dirent->ino = 0;  // RAMFS files don't have inodes in this implementation
        file->offset++;
        entries_read++;
    }

    return entries_read;
}

/*
 * ramfs_lookup - Searches the static file array for a matching filename.
 */
struct vfs_vnode* ramfs_lookup(struct vfs_vnode* dir, const char* filename)
{
    if (dir->type != VFS_VNODE_TYPE_DIR)
    {
        return NULL;
    }

    for (int i = 0; i < ramfs_file_count; i++)
    {
        if (strcmp(filename, ramfs_files[i].name) == 0)
        {
            atomic_inc(&ramfs_files[i].node->refcount);
            return ramfs_files[i].node;
        }
    }

    return NULL;
}

/*
 * ramfs_register_file - Creates a vnode and adds a file to the RAMFS registry.
 */
void ramfs_register_file(const char* name, const void* data, size_t size)
{
    if (ramfs_file_count >= RAMFS_MAX_FILES)
    {
        return;
    }

    ramfs_files[ramfs_file_count].name = name;
    ramfs_files[ramfs_file_count].data = data;
    ramfs_files[ramfs_file_count].size = size;

    struct vfs_vnode* vn = (struct vfs_vnode*)slab_alloc(sizeof(struct vfs_vnode));
    if (!vn)
    {
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

/* Hardcoded greeting message for the initial root filesystem */
static const char* ramfs_hello_txt = "Hello from the Raspberry Pi 4 RAMFS!\n";

/*
 * ramfs_init - Boot-time initialization of the RAM filesystem.
 */
void ramfs_init(void)
{
    ramfs_root_vnode = (struct vfs_vnode*)slab_alloc(sizeof(struct vfs_vnode));
    if (!ramfs_root_vnode)
    {
        return;
    }

    // Initialize directory operations table
    ramfs_dir_ops.lookup = ramfs_lookup;
    ramfs_dir_ops.readdir = ramfs_readdir;
    ramfs_dir_ops.read = NULL;
    ramfs_dir_ops.write = NULL;

    // Initialize file operations table
    ramfs_file_ops.lookup = NULL;
    ramfs_file_ops.read = ramfs_read;
    ramfs_file_ops.write = NULL;

    // Set up the root directory vnode
    ramfs_root_vnode->type = VFS_VNODE_TYPE_DIR;
    ramfs_root_vnode->ops = &ramfs_dir_ops;
    ramfs_root_vnode->internal_info = NULL;
    ramfs_root_vnode->parent = NULL;
    ramfs_root_vnode->file_size = 0;
    ramfs_root_vnode->refcount.counter = 1;

    // Mount RAMFS as the system root
    vfs_mount("/", ramfs_root_vnode);

    // Register initial files
    ramfs_register_file("hello.txt", ramfs_hello_txt, strlen(ramfs_hello_txt));
}
