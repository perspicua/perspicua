/*
 * vfs.c - Implementation of the Virtual Filesystem (VFS) layer.
 *
 * This file contains the generic filesystem logic, including mount point
 * management, path resolution, and the standard file operation dispatchers.
 */

#include "fs/vfs.h"

#include "uapi/errors.h"

#include "fs/devfs.h"
#include "mm/heap.h"
#include "mm/slab.h"
#include "core/lock.h"
#include "string.h"
#include "sched/process.h"
#include "stdio.h"

/* The global mount table and its synchronization lock */
static struct vfs_mount_entry vfs_mount_table[VFS_MAX_MOUNTS];
static int vfs_mount_count = 0;
static spinlock_t vfs_lock = SPINLOCK_INIT;

/* Internal path resolution helper */
static struct vfs_vnode* vfs_resolve_path_locked(const char* path, struct vfs_vnode* cwd, int* error);

/*
 * vfs_init - Boot-time initialization of the VFS structures.
 */
void vfs_init(void)
{
    unsigned long flags = spin_lock_irqsave(&vfs_lock);
    vfs_mount_count = 0;
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++)
    {
        memset(vfs_mount_table[i].path, 0, VFS_MAX_PATH_LEN);
        vfs_mount_table[i].root = (void*)0;
    }
    spin_unlock_irqrestore(&vfs_lock, flags);
}

/*
 * vfs_vnode_put - Release a reference to a vnode and free it if necessary.
 */
void vfs_vnode_put(struct vfs_vnode* node)
{
    if (!node)
    {
        return;
    }

    if (atomic_dec_and_test(&node->refcount))
    {
        struct vfs_vnode* parent = node->parent;
        slab_free(node);

        /* Recursively release the parent vnode reference */
        if (parent)
        {
            vfs_vnode_put(parent);
        }
    }
}

/*
 * vfs_mount - Mounts a new filesystem at the specified path.
 */
int vfs_mount(const char* path, struct vfs_vnode* root)
{
    if (!path || path[0] != '/' || !root)
    {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    unsigned long flags = spin_lock_irqsave(&vfs_lock);

    if (vfs_mount_count >= VFS_MAX_MOUNTS)
    {
        spin_unlock_irqrestore(&vfs_lock, flags);
        return -PERS_ERR_OUT_OF_RESOURCES;
    }

    /* Check for duplicate mount paths */
    for (int i = 0; i < vfs_mount_count; i++)
    {
        if (strcmp(path, vfs_mount_table[i].path) == 0)
        {
            spin_unlock_irqrestore(&vfs_lock, flags);
            return -PERS_ERR_ALREADY_EXISTS;
        }
    }

    /* Resolve the parent vnode for non-root mounts */
    if (strcmp(path, "/") != 0)
    {
        char parent_path[VFS_MAX_PATH_LEN];
        strncpy(parent_path, path, VFS_MAX_PATH_LEN - 1);
        parent_path[VFS_MAX_PATH_LEN - 1] = '\0';

        char* last_slash = strrchr(parent_path, '/');
        if (last_slash == parent_path)
        {
            parent_path[1] = '\0';
        }
        else if (last_slash)
        {
            *last_slash = '\0';
        }

        int err;
        struct vfs_vnode* parent = vfs_resolve_path_locked(parent_path, (void*)0, &err);
        if (parent)
        {
            root->parent = parent;
        }
    }
    else
    {
        root->parent = (void*)0;
    }

    /* Register the new mount entry */
    strncpy(vfs_mount_table[vfs_mount_count].path, path, VFS_MAX_PATH_LEN);
    vfs_mount_table[vfs_mount_count].root = root;
    atomic_inc(&root->refcount);
    vfs_mount_count++;

    spin_unlock_irqrestore(&vfs_lock, flags);
    return PERS_SUCCESS;
}

/*
 * vfs_unmount - Removes a mount point from the VFS namespace.
 */
int vfs_unmount(const char* path)
{
    if (!path)
    {
        return -PERS_ERR_NOT_FOUND;
    }

    unsigned long flags = spin_lock_irqsave(&vfs_lock);
    for (int i = 0; i < vfs_mount_count; i++)
    {
        if (strcmp(path, vfs_mount_table[i].path) == 0)
        {
            struct vfs_vnode* root = vfs_mount_table[i].root;

            /* Move the last entry into the current slot to maintain density */
            strncpy(vfs_mount_table[i].path, vfs_mount_table[vfs_mount_count - 1].path, VFS_MAX_PATH_LEN);
            vfs_mount_table[i].root = vfs_mount_table[vfs_mount_count - 1].root;

            memset(vfs_mount_table[vfs_mount_count - 1].path, 0, VFS_MAX_PATH_LEN);
            vfs_mount_table[vfs_mount_count - 1].root = (void*)0;

            vfs_mount_count--;
            spin_unlock_irqrestore(&vfs_lock, flags);

            vfs_vnode_put(root);
            return PERS_SUCCESS;
        }
    }

    spin_unlock_irqrestore(&vfs_lock, flags);
    return -PERS_ERR_NOT_FOUND;
}

/*
 * find_mount - Internal helper to find the longest matching mount point prefix.
 */
static struct vfs_mount_entry* find_mount(const char* path, int* error)
{
    int longest_match_index = -1;
    size_t longest_match_len = 0;

    for (int i = 0; i < vfs_mount_count; i++)
    {
        size_t len = strlen(vfs_mount_table[i].path);

        if (strncmp(vfs_mount_table[i].path, path, len) == 0)
        {
            /* Ensure the match is a full path component */
            if (len > 1 && path[len] != '\0' && path[len] != '/')
            {
                continue;
            }

            if (len >= longest_match_len)
            {
                longest_match_len = len;
                longest_match_index = i;
            }
        }
    }

    if (longest_match_index == -1)
    {
        *error = -PERS_ERR_NOT_FOUND;
        return (void*)0;
    }

    *error = PERS_SUCCESS;
    return &vfs_mount_table[longest_match_index];
}

/*
 * vfs_resolve_path_locked - Core path traversal implementation.
 */
static struct vfs_vnode* vfs_resolve_path_locked(const char* path, struct vfs_vnode* cwd, int* error)
{
    struct vfs_vnode* curr = (void*)0;
    char filepath[VFS_MAX_PATH_LEN];
    const char* path_remainder = path;

    /* Handle absolute path by starting from the root mount */
    if (path[0] == '/')
    {
        struct vfs_mount_entry* best_match = find_mount(path, error);
        if (!best_match)
        {
            return (void*)0;
        }

        curr = best_match->root;
        atomic_inc(&curr->refcount);
        size_t len = strlen(best_match->path);

        if (strcmp(path, best_match->path) == 0)
        {
            *error = PERS_SUCCESS;
            return curr;
        }

        path_remainder = path + len;
        if (*path_remainder == '/')
        {
            path_remainder++;
        }
    }
    else
    {
        /* Start traversal from the current working directory or default root */
        if (!cwd)
        {
            struct vfs_mount_entry* root_match = find_mount("/", error);
            if (!root_match)
            {
                return (void*)0;
            }
            curr = root_match->root;
        }
        else
        {
            curr = cwd;
        }
        atomic_inc(&curr->refcount);
    }

    if (*path_remainder == '\0')
    {
        *error = PERS_SUCCESS;
        return curr;
    }

    strncpy(filepath, path_remainder, VFS_MAX_PATH_LEN - 1);
    filepath[VFS_MAX_PATH_LEN - 1] = '\0';

    char* saveptr;
    char* token = strtok_r(filepath, "/", &saveptr);

    while (token)
    {
        struct vfs_vnode* next = (void*)0;

        if (strcmp(token, ".") == 0)
        {
            next = curr;
            atomic_inc(&next->refcount);
        }
        else if (strcmp(token, "..") == 0)
        {
            next = curr->parent ? curr->parent : curr;
            atomic_inc(&next->refcount);
        }
        else
        {
            /* Delegate traversal to the filesystem driver's lookup operation */
            if (!curr->ops || !curr->ops->lookup)
            {
                vfs_vnode_put(curr);
                *error = -PERS_ERR_NOT_A_DIRECTORY;
                return (void*)0;
            }

            next = curr->ops->lookup(curr, token);
            if (!next)
            {
                vfs_vnode_put(curr);
                *error = -PERS_ERR_NOT_FOUND;
                return (void*)0;
            }
        }

        vfs_vnode_put(curr);
        curr = next;
        token = strtok_r((void*)0, "/", &saveptr);
    }

    *error = PERS_SUCCESS;
    return curr;
}

/*
 * vfs_resolve_path - Public thread-safe path resolution.
 */
struct vfs_vnode* vfs_resolve_path(const char* path, struct vfs_vnode* cwd, int* error)
{
    unsigned long flags = spin_lock_irqsave(&vfs_lock);
    struct vfs_vnode* res = vfs_resolve_path_locked(path, cwd, error);
    spin_unlock_irqrestore(&vfs_lock, flags);
    return res;
}

/*
 * vfs_open_pid - Specialized open for cross-process operations.
 */
int vfs_open_pid(const char* path, int flags, uint32_t pid)
{
    int error = 0;
    if (pid >= PROCESS_TABLE_SIZE)
    {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }

    struct vfs_vnode* node = vfs_resolve_path(path, process_table[pid].cwd, &error);
    if (!node)
    {
        return error;
    }

    /* Check for duplicate opens of the same underlying object */
    spin_lock(&process_table[pid].fd_lock);
    for (size_t i = 0; i < VFS_MAX_FDS; i++)
    {
        struct vfs_file* f = process_table[pid].fd_table[i];
        if (f && f->node->internal_info == node->internal_info && f->node->ops == node->ops)
        {
            if (node->type != VFS_VNODE_TYPE_DEVICE)
            {
                spin_unlock(&process_table[pid].fd_lock);
                vfs_vnode_put(node);
                return -PERS_ERR_ALREADY_EXISTS;
            }
        }
    }
    spin_unlock(&process_table[pid].fd_lock);

    struct vfs_file* new_file = (struct vfs_file*)slab_alloc(sizeof(struct vfs_file));
    if (!new_file)
    {
        vfs_vnode_put(node);
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    new_file->node = node;
    new_file->offset = 0;
    new_file->flags = flags;
    new_file->refcount.counter = 1;

    int slot = -1;
    spin_lock(&process_table[pid].fd_lock);
    for (size_t i = 0; i < VFS_MAX_FDS; i++)
    {
        if (!process_table[pid].fd_table[i])
        {
            process_table[pid].fd_table[i] = new_file;
            slot = (int)i;
            break;
        }
    }
    spin_unlock(&process_table[pid].fd_lock);

    if (slot == -1)
    {
        vfs_vnode_put(node);
        slab_free(new_file);
        return -PERS_ERR_OUT_OF_RESOURCES;
    }
    return slot;
}

/*
 * vfs_open - Primary file open entry point.
 */
int vfs_open(const char* path, int flags)
{
    int pid = process_find_current();
    if (pid < 0)
    {
        return pid;
    }
    return vfs_open_pid(path, flags, (uint32_t)pid);
}

/*
 * vfs_close - Implementation of the file close operation.
 */
int vfs_close(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FDS)
    {
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    int pid = process_find_current();
    if (pid < 0)
    {
        return pid;
    }

    struct process* p = &process_table[pid];
    spin_lock(&p->fd_lock);
    struct vfs_file* f = p->fd_table[fd];

    if (!f)
    {
        spin_unlock(&p->fd_lock);
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    p->fd_table[fd] = (void*)0;
    spin_unlock(&p->fd_lock);

    if (atomic_dec_and_test(&f->refcount))
    {
        /* Perform filesystem-specific cleanup before freeing the file object */
        if (f->node->ops && f->node->ops->close)
        {
            f->node->ops->close(f);
        }

        vfs_vnode_put(f->node);
        slab_free(f);
    }

    return PERS_SUCCESS;
}

/*
 * vfs_lseek - Repositions the file offset.
 */
vfs_off_t vfs_lseek(int fd, vfs_off_t offset, int whence)
{
    if (fd < 0 || fd >= VFS_MAX_FDS)
    {
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    int pid = process_find_current();
    if (pid < 0)
    {
        return pid;
    }

    struct process* p = &process_table[pid];
    spin_lock(&p->fd_lock);
    struct vfs_file* f = p->fd_table[fd];
    if (!f)
    {
        spin_unlock(&p->fd_lock);
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    vfs_off_t new_offset;
    switch (whence)
    {
    case VFS_SEEK_SET:
        new_offset = offset;
        break;
    case VFS_SEEK_CUR:
        new_offset = f->offset + offset;
        break;
    case VFS_SEEK_END:
        new_offset = f->node->file_size + offset;
        break;
    default:
        spin_unlock(&p->fd_lock);
        return -PERS_ERR_NOT_IMPLEMENTED;
    }

    if (new_offset < 0)
    {
        spin_unlock(&p->fd_lock);
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    f->offset = new_offset;
    spin_unlock(&p->fd_lock);
    return new_offset;
}

/*
 * vfs_read - Generic read dispatcher.
 */
int vfs_read(int fd, void* buffer, size_t count)
{
    if (fd < 0 || fd >= VFS_MAX_FDS)
    {
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    int pid = process_find_current();
    if (pid < 0)
    {
        return pid;
    }

    struct process* p = &process_table[pid];
    spin_lock(&p->fd_lock);
    struct vfs_file* f = p->fd_table[fd];
    if (!f)
    {
        spin_unlock(&p->fd_lock);
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }
    atomic_inc(&f->refcount);
    spin_unlock(&p->fd_lock);

    /* Validate access permissions and operation availability */
    int mode = f->flags & VFS_O_ACCMODE;
    if ((mode != VFS_O_RDONLY && mode != VFS_O_RDWR) || !f->node->ops->read)
    {
        atomic_dec_and_test(&f->refcount);
        return -PERS_ERR_PERMISSION_DENIED;
    }

    int bytes = f->node->ops->read(f, buffer, count);
    atomic_dec_and_test(&f->refcount);
    return bytes;
}

/*
 * vfs_readdir - Generic readdir dispatcher.
 */
int vfs_readdir(int fd, void* buffer, size_t count)
{
    if (fd < 0 || fd >= VFS_MAX_FDS)
    {
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    int pid = process_find_current();
    if (pid < 0)
    {
        return pid;
    }

    struct process* p = &process_table[pid];
    spin_lock(&p->fd_lock);
    struct vfs_file* f = p->fd_table[fd];
    if (!f)
    {
        spin_unlock(&p->fd_lock);
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }
    atomic_inc(&f->refcount);
    spin_unlock(&p->fd_lock);

    /* Ensure it is a directory and the operation is supported */
    if (f->node->type != VFS_VNODE_TYPE_DIR || !f->node->ops->readdir)
    {
        atomic_dec_and_test(&f->refcount);
        return -PERS_ERR_NOT_A_DIRECTORY;
    }

    int res = f->node->ops->readdir(f, buffer, count);
    atomic_dec_and_test(&f->refcount);
    return res;
}

/*
 * vfs_write - Generic write dispatcher.
 */
int vfs_write(int fd, const void* buffer, size_t count)
{
    if (fd < 0 || fd >= VFS_MAX_FDS)
    {
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    int pid = process_find_current();
    if (pid < 0)
    {
        return pid;
    }

    struct process* p = &process_table[pid];
    spin_lock(&p->fd_lock);
    struct vfs_file* f = p->fd_table[fd];
    if (!f)
    {
        spin_unlock(&p->fd_lock);
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }
    atomic_inc(&f->refcount);
    spin_unlock(&p->fd_lock);

    /* Validate access permissions and operation availability */
    int mode = f->flags & VFS_O_ACCMODE;
    if ((mode != VFS_O_WRONLY && mode != VFS_O_RDWR) || !f->node->ops->write)
    {
        atomic_dec_and_test(&f->refcount);
        return -PERS_ERR_PERMISSION_DENIED;
    }

    if (f->flags & VFS_O_APPEND)
    {
        f->offset = f->node->file_size;
    }

    int bytes = f->node->ops->write(f, buffer, count);
    atomic_dec_and_test(&f->refcount);
    return bytes;
}

/*
 * vfs_dup2 - Duplicates a file descriptor to a specific new descriptor.
 */
int vfs_dup2(int oldfd, int newfd)
{
    if (oldfd < 0 || oldfd >= VFS_MAX_FDS || newfd < 0 || newfd >= VFS_MAX_FDS)
    {
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    int pid = process_find_current();
    if (pid < 0)
    {
        return pid;
    }

    struct process* p = &process_table[pid];
    struct vfs_file* to_free = (void*)0;

    spin_lock(&p->fd_lock);
    struct vfs_file* old_f = p->fd_table[oldfd];
    if (!old_f)
    {
        spin_unlock(&p->fd_lock);
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    if (oldfd == newfd)
    {
        spin_unlock(&p->fd_lock);
        return newfd;
    }

    struct vfs_file* existing_f = p->fd_table[newfd];
    if (existing_f)
    {
        if (atomic_dec_and_test(&existing_f->refcount))
        {
            to_free = existing_f;
        }
    }

    p->fd_table[newfd] = old_f;
    atomic_inc(&old_f->refcount);
    spin_unlock(&p->fd_lock);

    if (to_free)
    {
        if (to_free->node->ops && to_free->node->ops->close)
        {
            to_free->node->ops->close(to_free);
        }
        vfs_vnode_put(to_free->node);
        slab_free(to_free);
    }

    return newfd;
}

int vfs_chdir(const char* kpath){
    int pid = process_find_current();
    if (pid < 0)
    {
        return pid;
    }

    struct process* p = &process_table[pid];
    int error = 0;
    
    struct vfs_vnode* node = vfs_resolve_path(kpath, p->cwd, &error);
    if (!node)
    {
        return error;
    }

    if (node->type != VFS_VNODE_TYPE_DIR)
    {
        vfs_vnode_put(node);
        return -PERS_ERR_NOT_A_DIRECTORY;
    }

    spin_lock(&p->fd_lock);
    if (p->cwd)
    {
        vfs_vnode_put(p->cwd);
    }
    p->cwd = node;
    // atomic_inc(&node->refcount);
    spin_unlock(&p->fd_lock);

    return PERS_SUCCESS;
}
