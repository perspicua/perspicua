#include "kernel/ramfs.h"
#include "kernel/heap.h"
#include "kernel/vfs.h"
#include "lib/string.h"

const char* hello_file_data = "Hello from the Raspberry Pi 4 RAMFS!\n";

struct vnode* ramfs_root_vnode;
struct vnode* ramfs_hello_vnode;

struct vnode_ops ramfs_dir_ops;
struct vnode_ops ramfs_file_ops;

int ramfs_read(struct file* file, void* buffer, size_t size)
{
    size_t file_len = strlen(hello_file_data);

    if (file->offset >= file_len)
        return 0; // eof

    size_t bytes_to_read = size;
    if (file->offset + bytes_to_read > file_len)
        bytes_to_read = file_len - file->offset;

    memcpy(buffer, hello_file_data + file->offset, bytes_to_read);

    return (int)bytes_to_read;
}
struct vnode* ramfs_lookup(struct vnode* dir, const char* filename)
{
    if (dir != ramfs_root_vnode)
        return NULL;

    if (strcmp(filename, "hello.txt") == 0)
        return ramfs_hello_vnode;

    return NULL;
}

void ramfs_init(void)
{
    ramfs_root_vnode = (struct vnode*)kmalloc(sizeof(struct vnode));
    ramfs_hello_vnode = (struct vnode*)kmalloc(sizeof(struct vnode));

    ramfs_dir_ops.lookup = ramfs_lookup;
    ramfs_dir_ops.read = NULL;
    ramfs_dir_ops.write = NULL;

    ramfs_file_ops.lookup = NULL;
    ramfs_file_ops.read = ramfs_read;
    ramfs_file_ops.write = NULL;

    ramfs_root_vnode->type = VNODE_TYPE_DIR;
    ramfs_root_vnode->ops = &ramfs_dir_ops;

    ramfs_hello_vnode->type = VNODE_TYPE_REGULAR;
    ramfs_hello_vnode->ops = &ramfs_file_ops;
    ramfs_hello_vnode->filesize = strlen(hello_file_data);

    vfs_mount("/", ramfs_root_vnode);
}
