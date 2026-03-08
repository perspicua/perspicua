#include "kernel/vfs.h"
#include "lib/string.h"
#include "kernel/heap.h"
#include "kernel/process.h"

struct vnode* vfs_root;

void vfs_init(void)
{
    vfs_root = NULL;
}

void vfs_set_root(struct vnode* root)
{
    vfs_root = root;
}

struct vnode* vfs_resolve_path(const char* path)
{
    if (vfs_root == NULL || path == NULL)
        return NULL;

    if (strcmp(path, "/") == 0)
        return vfs_root;

    struct vnode* target_vnode = vfs_root;

    char filepath[MAX_PATH_LEN];
    strncpy(filepath, path, MAX_PATH_LEN - 1);
    filepath[MAX_PATH_LEN - 1] = '\0';
    char* delimiter = "/";
    char* saveptr;
    char* token = strtok_r(filepath, delimiter, &saveptr);

    while (token != NULL)
    {
        if (target_vnode->ops->lookup == NULL)
            return NULL;

        target_vnode = target_vnode->ops->lookup(target_vnode, token);

        if (target_vnode == NULL)
            return NULL;

        token = strtok_r(NULL, delimiter, &saveptr);
    }
    return target_vnode;
}

int vfs_open(const char* path, int flags)
{
    struct vnode* node = vfs_resolve_path(path);
    if (node == NULL)
        return -1;

    struct file* new_file = (struct file*)kmalloc(sizeof(struct file));

    new_file->node = node;
    new_file->offset = 0;
    new_file->flags = flags;

    int curr_process_pid = process_find_current();
    int ok = -1;
    for (size_t i = 0; i < MAX_FDS && ok == -1; i++)
    {
        if (process_table[curr_process_pid].fd_table[i] == NULL)
        {
            process_table[curr_process_pid].fd_table[i] = new_file;
            ok = (int)i;
        }
    }
    if (ok == -1)
        kfree(new_file);
    return ok;
}

int vfs_close(int fd)
{
    if (fd < 0 || fd >= MAX_FDS)
        return -1;

    int curr_process_pid = process_find_current();
    if (process_table[curr_process_pid].fd_table[fd] == NULL)
        return -1;

    kfree(process_table[curr_process_pid].fd_table[fd]);
    process_table[curr_process_pid].fd_table[fd] = NULL;

    return 0;
}

int vfs_read(int fd, void* buffer, size_t count)
{
    if (fd < 0 || fd >= MAX_FDS)
        return -1;

    int curr_process_pid = process_find_current();
    if (process_table[curr_process_pid].fd_table[fd] == NULL)
        return -1;

    struct file* file_to_read = process_table[curr_process_pid].fd_table[fd];
    if (file_to_read->node->ops->read == NULL)
        return -1;

    int bytes_read = file_to_read->node->ops->read(file_to_read, buffer, count);
    if (bytes_read > 0)
        file_to_read->offset += bytes_read;

    return bytes_read;
}

int vfs_write(int fd, const void* buffer, size_t count)
{
    if (fd < 0 || fd >= MAX_FDS)
        return -1;

    int curr_process_pid = process_find_current();
    if (process_table[curr_process_pid].fd_table[fd] == NULL)
        return -1;

    struct file* file_to_write = process_table[curr_process_pid].fd_table[fd];
    if (file_to_write->node->ops->write == NULL)
        return -1;

    int bytes_written = file_to_write->node->ops->write(file_to_write, buffer, count);
    if (bytes_written > 0)
        file_to_write->offset += bytes_written;

    return bytes_written;
}
