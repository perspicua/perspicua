#include "ramfs.h"
#include "heap.h"
#include "vfs.h"
#include "slab.h"
#include "string.h"
#include "stdio.h"

struct ramfs_file_data
{
    const char* name;
    const void* data;
    size_t size;
};

#define MAX_RAMFS_FILES 16
static struct ramfs_file_data ramfs_files[MAX_RAMFS_FILES];
static int ramfs_file_count = 0;

struct vnode* ramfs_root_vnode;
struct vnode_ops ramfs_dir_ops;
struct vnode_ops ramfs_file_ops;

int ramfs_read(struct file* file, void* buffer, size_t size)
{
    struct ramfs_file_data* data = (struct ramfs_file_data*)file->node->internal_info;
    if (!data)
        return -1;

    if (file->offset >= (off_t)data->size)
        return 0; // eof

    size_t bytes_to_read = size;
    if (file->offset + (off_t)bytes_to_read > (off_t)data->size)
        bytes_to_read = data->size - (size_t)file->offset;

    memcpy(buffer, (const char*)data->data + file->offset, bytes_to_read);

    return (int)bytes_to_read;
}

// REMINDER TO FREE VNODE WHEN FILE DELETED
struct vnode* ramfs_lookup(struct vnode* dir, const char* filename)
{
    if (dir->type != VNODE_TYPE_DIR)
        return NULL;

    for (int i = 0; i < ramfs_file_count; i++)
    {
        if (strcmp(filename, ramfs_files[i].name) == 0)
        {
            struct vnode* vn =
                (struct vnode*)slab_alloc(sizeof(struct vnode)); // allocd here so it can be freed when file is deleted
            vn->type = VNODE_TYPE_REGULAR;
            vn->ops = &ramfs_file_ops;
            vn->filesize = (off_t)ramfs_files[i].size;
            vn->internal_info = &ramfs_files[i];
            vn->refcount.counter = 1;
            return vn;
        }
    }

    return NULL;
}

void ramfs_register_file(const char* name, const void* data, size_t size)
{
    if (ramfs_file_count >= MAX_RAMFS_FILES)
        return;
    ramfs_files[ramfs_file_count].name = name;
    ramfs_files[ramfs_file_count].data = data;
    ramfs_files[ramfs_file_count].size = size;
    ramfs_file_count++;
    printf("[RAMFS ] Registered file: %s (%u bytes)\n", name, size);
}

const char* hello_txt = "Hello from the Raspberry Pi 4 RAMFS!\n";

void ramfs_init(void)
{
    ramfs_root_vnode = (struct vnode*)slab_alloc(sizeof(struct vnode));

    ramfs_dir_ops.lookup = ramfs_lookup;
    ramfs_dir_ops.read = NULL;
    ramfs_dir_ops.write = NULL;

    ramfs_file_ops.lookup = NULL;
    ramfs_file_ops.read = ramfs_read;
    ramfs_file_ops.write = NULL;

    ramfs_root_vnode->type = VNODE_TYPE_DIR;
    ramfs_root_vnode->ops = &ramfs_dir_ops;
    ramfs_root_vnode->internal_info = NULL;
    ramfs_root_vnode->refcount.counter = 1;

    vfs_mount("/", ramfs_root_vnode);

    ramfs_register_file("hello.txt", hello_txt, strlen(hello_txt));
}
