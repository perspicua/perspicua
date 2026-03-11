#include "vfs.h"
#include "devfs.h"
#include "string.h"
#include "heap.h"
#include "process.h"
#include "slab.h"
#include "stdio.h"
#include "types.h"

static struct mount_entry mount_table[MAX_MOUNTS];
static int mount_count = 0;
static spinlock_t vfs_lock = SPINLOCK_INIT;

void vfs_init(void)
{
    spin_lock(&vfs_lock);
    mount_count = 0;
    for (size_t i = 0; i < MAX_MOUNTS; i++)
    {
        memset(mount_table[i].path, 0, MAX_PATH_LEN);
        mount_table[i].root = NULL;
    }
    spin_unlock(&vfs_lock);
}

int vfs_mount(const char* path, struct vnode* root)
{
    if (path == NULL || path[0] != '/' || root == NULL)
        return -1;

    spin_lock(&vfs_lock);
    if (mount_count >= MAX_MOUNTS)
    {
        spin_unlock(&vfs_lock);
        return -1;
    }

    for (int i = 0; i < mount_count; i++)
    {
        if (strcmp(path, mount_table[i].path) == 0)
        {
            spin_unlock(&vfs_lock);
            return -1;
        }
    }

    strncpy(mount_table[mount_count].path, path, MAX_PATH_LEN);
    mount_table[mount_count].root = root;
    atomic_inc(&root->refcount);
    mount_count++;
    spin_unlock(&vfs_lock);
    return 0;
}

int vfs_unmount(const char* path)
{
    spin_lock(&vfs_lock);
    for (int i = 0; i < mount_count; i++)
    {
        if (strcmp(path, mount_table[i].path) == 0)
        {
            struct vnode* root = mount_table[i].root;
            strncpy(mount_table[i].path, mount_table[mount_count - 1].path, MAX_PATH_LEN);
            mount_table[i].root = mount_table[mount_count - 1].root;

            memset(mount_table[mount_count - 1].path, 0, MAX_PATH_LEN);
            mount_table[mount_count - 1].root = NULL;

            mount_count--;
            spin_unlock(&vfs_lock);

            atomic_dec_and_test(&root->refcount);
            return 0;
        }
    }
    spin_unlock(&vfs_lock);
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
    spin_lock(&vfs_lock);
    struct mount_entry* best_match = find_mount(path);
    if (best_match == NULL)
    {
        spin_unlock(&vfs_lock);
        return NULL;
    }

    struct vnode* curr = best_match->root;
    atomic_inc(&curr->refcount);
    size_t len = strlen(best_match->path);

    // exact match
    if (strcmp(path, best_match->path) == 0)
    {
        spin_unlock(&vfs_lock);
        return curr;
    }

    char* path_remainder = (char*)path + len;
    if (*path_remainder == '/')
        path_remainder++;

    if (*path_remainder == '\0')
    {
        spin_unlock(&vfs_lock);
        return curr;
    }

    char filepath[MAX_PATH_LEN];
    strncpy(filepath, path_remainder, MAX_PATH_LEN - 1);
    filepath[MAX_PATH_LEN - 1] = '\0';

    char* saveptr;
    char* token = strtok_r(filepath, "/", &saveptr);

    while (token != NULL)
    {
        if (curr->ops->lookup == NULL)
        {
            atomic_dec_and_test(&curr->refcount);
            spin_unlock(&vfs_lock);
            return NULL;
        }

        struct vnode* next = curr->ops->lookup(curr, token);
        if (next == NULL)
        {
            atomic_dec_and_test(&curr->refcount);
            spin_unlock(&vfs_lock);
            return NULL;
        }

        struct vnode* old = curr;
        curr = next;
        atomic_inc(&curr->refcount);
        atomic_dec_and_test(&old->refcount);

        token = strtok_r(NULL, "/", &saveptr);
    }
    spin_unlock(&vfs_lock);
    return curr;
}

int vfs_open_pid(const char* path, int flags, uint32_t pid)
{
    struct vnode* node = vfs_resolve_path(path);
    if (node == NULL)
        return -1;

    if (pid >= PROCESS_TABLE_SIZE)
    {
        atomic_dec_and_test(&node->refcount);
        return -1;
    }

    struct file* new_file = (struct file*)slab_alloc(sizeof(struct file));
    if (!new_file)
    {
        atomic_dec_and_test(&node->refcount);
        return -1;
    }

    new_file->node = node;
    new_file->offset = 0;
    new_file->flags = flags;
    new_file->refcount.counter = 1;

    int ok = -1;
    spin_lock(&process_table[pid].fd_lock);
    for (size_t i = 0; i < MAX_FDS && ok == -1; i++)
    {
        if (process_table[pid].fd_table[i] == NULL)
        {
            process_table[pid].fd_table[i] = new_file;
            ok = (int)i;
        }
    }
    spin_unlock(&process_table[pid].fd_lock);

    if (ok == -1)
    {
        atomic_dec_and_test(&node->refcount);
        slab_free(new_file);
    }
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

    struct process* p = &process_table[curr_process_pid];
    spin_lock(&p->fd_lock);
    struct file* f = p->fd_table[fd];
    if (f == NULL)
    {
        spin_unlock(&p->fd_lock);
        return -1;
    }
    p->fd_table[fd] = NULL;
    spin_unlock(&p->fd_lock);

    if (atomic_dec_and_test(&f->refcount))
    {
        atomic_dec_and_test(&f->node->refcount);
        slab_free(f);
    }

    return 0;
}

off_t vfs_lseek(int fd, off_t offset, int whence)
{
    if (fd < 0 || fd >= MAX_FDS)
        return -1;

    int curr_process_pid = process_find_current();
    if (curr_process_pid < 0 || curr_process_pid >= PROCESS_TABLE_SIZE)
        return -1;

    struct process* p = &process_table[curr_process_pid];
    spin_lock(&p->fd_lock);
    struct file* f = p->fd_table[fd];
    if (f == NULL)
    {
        spin_unlock(&p->fd_lock);
        return -1;
    }

    off_t new_offset = f->offset;
    switch (whence)
    {
    case SEEK_SET:
        new_offset = offset;
        break;
    case SEEK_CUR:
        new_offset = f->offset + offset;
        break;
    case SEEK_END:
        new_offset = f->node->filesize + offset;
        break;
    default:
        spin_unlock(&p->fd_lock);
        return -1;
    }

    if (new_offset < 0)
    {
        spin_unlock(&p->fd_lock);
        return -1;
    }

    f->offset = new_offset;
    off_t res = f->offset;
    spin_unlock(&p->fd_lock);
    return res;
}

int vfs_read(int fd, void* buffer, size_t count)
{
    if (fd < 0 || fd >= MAX_FDS)
        return -1;

    int curr_process_pid = process_find_current();
    if (curr_process_pid < 0 || curr_process_pid >= PROCESS_TABLE_SIZE)
        return -1;

    struct process* p = &process_table[curr_process_pid];
    spin_lock(&p->fd_lock);
    struct file* f = p->fd_table[fd];
    if (f == NULL)
    {
        spin_unlock(&p->fd_lock);
        return -1;
    }
    atomic_inc(&f->refcount);
    spin_unlock(&p->fd_lock);

    // check read flags
    int access_mode = f->flags & O_ACCMODE;
    if ((access_mode != O_RDONLY && access_mode != O_RDWR) || f->node->ops->read == NULL)
    {
        atomic_dec_and_test(&f->refcount);
        return -1;
    }

    int bytes_read = f->node->ops->read(f, buffer, count);
    if (bytes_read > 0)
    {
        spin_lock(&p->fd_lock);
        f->offset += bytes_read;
        spin_unlock(&p->fd_lock);
    }

    atomic_dec_and_test(&f->refcount);
    return bytes_read;
}

int vfs_write(int fd, const void* buffer, size_t count)
{
    if (fd < 0 || fd >= MAX_FDS)
        return -1;

    int curr_process_pid = process_find_current();
    if (curr_process_pid < 0 || curr_process_pid >= PROCESS_TABLE_SIZE)
        return -1;

    struct process* p = &process_table[curr_process_pid];
    spin_lock(&p->fd_lock);
    struct file* f = p->fd_table[fd];
    if (f == NULL)
    {
        spin_unlock(&p->fd_lock);
        return -1;
    }
    atomic_inc(&f->refcount);
    spin_unlock(&p->fd_lock);

    // check write flags
    int access_mode = f->flags & O_ACCMODE;
    if ((access_mode != O_WRONLY && access_mode != O_RDWR) || f->node->ops->write == NULL)
    {
        atomic_dec_and_test(&f->refcount);
        return -1;
    }

    if (f->flags & O_APPEND)
        f->offset = f->node->filesize;

    int bytes_written = f->node->ops->write(f, buffer, count);
    if (bytes_written > 0)
    {
        spin_lock(&p->fd_lock);
        f->offset += bytes_written;
        spin_unlock(&p->fd_lock);
    }

    atomic_dec_and_test(&f->refcount);
    return bytes_written;
}
