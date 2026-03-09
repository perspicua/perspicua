#include "kernel/vfs.h"
#include "kernel/devfs.h"
#include "lib/string.h"
#include "kernel/heap.h"
#include "kernel/process.h"
#include "lib/stdio.h"
#include "lib/types.h"

static struct mount_entry mount_table[MAX_MOUNTS];
static int mount_count = 0;

void vfs_init(void)
{
    mount_count = 0;
    for (size_t i = 0; i < MAX_MOUNTS; i++)
    {
        memset(mount_table[i].path, 0, MAX_PATH_LEN);
        mount_table[i].root = NULL;
    }
}

int vfs_mount(const char* path, struct vnode* root)
{
    if (path == NULL)
        return -1;

    if (path[0] != '/')
        return -1;

    if (root == NULL)
        return -1;

    if (mount_count >= MAX_MOUNTS)
        return -1;

    for (int i = 0; i < mount_count; i++)
        if (strcmp(path, mount_table[i].path) == 0)
            return -1;

    strncpy(mount_table[mount_count].path, path, MAX_PATH_LEN);
    mount_table[mount_count].root = root;
    mount_count++;
    return 0;
}

int vfs_unmount(const char* path)
{
    for (int i = 0; i < mount_count; i++)
    {
        if (strcmp(path, mount_table[i].path) == 0)
        {
            strncpy(mount_table[i].path, mount_table[mount_count - 1].path, MAX_PATH_LEN);
            mount_table[i].root = mount_table[mount_count - 1].root;

            memset(mount_table[mount_count - 1].path, 0, MAX_PATH_LEN);
            mount_table[mount_count - 1].root = NULL;

            mount_count--;
            return 0;
        }
    }
    return -1;
}

static struct mount_entry* find_mount(const char* path)
{
    int longest_match_index = -1;
    size_t longest_match_len = 0;
    for (int i = 0; i < mount_count; i++)
    {
        size_t len = strlen(mount_table[i].path);

        if (strncmp(mount_table[i].path, path, len) == 0)
        {
            if (len > 1 && path[len] != '\0' && path[len] != '/')
                continue;

            if (len >= longest_match_len)
            {
                longest_match_len = len;
                longest_match_index = i;
            }
        }
    }

    if (longest_match_index == -1)
        return NULL;

    return &mount_table[longest_match_index];
}

struct vnode* vfs_resolve_path(const char* path)
{
    struct mount_entry* best_match = find_mount(path);
    if (best_match == NULL)
        return NULL;

    // exact match
    if (strcmp(path, best_match->path) == 0)
        return best_match->root;

    struct vnode* target_vnode = best_match->root;

    size_t len = strlen(best_match->path);
    char* path_remainder = (char*)path + len;
    if (*path_remainder == '/')
        path_remainder++;

    char filepath[MAX_PATH_LEN];
    strncpy(filepath, path_remainder, MAX_PATH_LEN - 1);
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

int vfs_open_pid(const char* path, int flags, uint32_t pid)
{
    struct vnode* node = vfs_resolve_path(path);

    if (node == NULL)
        return -1;

    if (pid >= PROCESS_TABLE_SIZE)
        return -1;
    struct file* new_file = (struct file*)kmalloc(sizeof(struct file));

    new_file->node = node;
    new_file->offset = 0;
    new_file->flags = flags;

    int ok = -1;
    for (size_t i = 0; i < MAX_FDS && ok == -1; i++)
    {
        if (process_table[pid].fd_table[i] == NULL)
        {
            process_table[pid].fd_table[i] = new_file;
            ok = (int)i;
        }
    }
    if (ok == -1)
        kfree(new_file);
    return ok;
}

int vfs_open(const char* path, int flags)
{
    int curr_process_pid = process_find_current();
    if (curr_process_pid < 0)
        return -1;
    return vfs_open_pid(path, flags, (uint32_t)curr_process_pid);
}

int vfs_close(int fd)
{
    if (fd < 0 || fd >= MAX_FDS)
        return -1;

    int curr_process_pid = process_find_current();
    if (curr_process_pid < 0 || curr_process_pid >= PROCESS_TABLE_SIZE)
        return -1;
    if (process_table[curr_process_pid].fd_table[fd] == NULL)
        return -1;

    kfree(process_table[curr_process_pid].fd_table[fd]);
    process_table[curr_process_pid].fd_table[fd] = NULL;

    return 0;
}

int vfs_lseek(int fd, int offset, int whence)
{
    if (fd < 0 || fd >= MAX_FDS)
        return -1;

    int curr_process_pid = process_find_current();
    if (curr_process_pid < 0 || curr_process_pid >= PROCESS_TABLE_SIZE)
        return -1;
    if (process_table[curr_process_pid].fd_table[fd] == NULL)
        return -1;

    struct file* f = process_table[curr_process_pid].fd_table[fd];

    uint32_t new_offset = f->offset;
    switch (whence)
    {
    case SEEK_SET:
        new_offset = (uint32_t)offset;
        break;
    case SEEK_CUR:
        new_offset = (uint32_t)((int)f->offset + offset);
        break;
    case SEEK_END:
        new_offset = (uint32_t)((int)f->node->filesize + offset);
        break;
    default:
        return -1;
    }

    f->offset = new_offset;
    return (int)f->offset;
}

int vfs_read(int fd, void* buffer, size_t count)
{
    if (fd < 0 || fd >= MAX_FDS)
        return -1;

    int curr_process_pid = process_find_current();
    if (curr_process_pid < 0 || curr_process_pid >= PROCESS_TABLE_SIZE)
        return -1;
    if (process_table[curr_process_pid].fd_table[fd] == NULL)
        return -1;

    struct file* file_to_read = process_table[curr_process_pid].fd_table[fd];

    // check read flags
    int access_mode = file_to_read->flags & O_ACCMODE;
    if (access_mode != O_RDONLY && access_mode != O_RDWR)
        return -1;

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

    if (curr_process_pid < 0 || curr_process_pid >= PROCESS_TABLE_SIZE)
        return -1;

    if (process_table[curr_process_pid].fd_table[fd] == NULL)
        return -1;

    struct file* file_to_write = process_table[curr_process_pid].fd_table[fd];

    // check write flags
    int access_mode = file_to_write->flags & O_ACCMODE;
    if (access_mode != O_WRONLY && access_mode != O_RDWR)
        return -1;

    if (file_to_write->node->ops->write == NULL)
        return -1;

    if (file_to_write->flags & O_APPEND)
        file_to_write->offset = (uint32_t)file_to_write->node->filesize;

    int bytes_written = file_to_write->node->ops->write(file_to_write, buffer, count);
    if (bytes_written > 0)
        file_to_write->offset += bytes_written;

    return bytes_written;
}
