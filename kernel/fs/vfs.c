#include "vfs.h"
#include "devfs.h"
#include "lock.h"
#include "string.h"
#include "heap.h"
#include "process.h"
#include "slab.h"
#include "stdio.h"
#include "types.h"
#include "uapi/errors.h"

static struct mount_entry mount_table[MAX_MOUNTS];
static int mount_count = 0;
static spinlock_t vfs_lock = SPINLOCK_INIT;

static struct vnode* vfs_resolve_path_locked(const char* path, struct vnode* cwd, int* error);

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

void vfs_vnode_put(struct vnode* node)
{
    if (!node)
        return;

    if (atomic_dec_and_test(&node->refcount))
    {
        struct vnode* parent = node->parent;
        slab_free(node);
        if (parent)
            vfs_vnode_put(parent);
    }
}

int vfs_mount(const char* path, struct vnode* root)
{
    if (path == NULL)
        return -PERS_ERR_INVALID_ARGUMENT;

    if (path[0] != '/')
        return -PERS_ERR_NOT_A_DIRECTORY;

    if (root == NULL)
        return -PERS_ERR_INVALID_ARGUMENT;

    spin_lock(&vfs_lock);
    if (mount_count >= MAX_MOUNTS)
    {
        spin_unlock(&vfs_lock);
        return -PERS_ERR_OUT_OF_RESOURCES;
    }

    for (int i = 0; i < mount_count; i++)
    {
        if (strcmp(path, mount_table[i].path) == 0)
        {
            spin_unlock(&vfs_lock);
            return -PERS_ERR_ALREADY_EXISTS;
        }
    }

    if (strcmp(path, "/") != 0)
    {
        char parent_path[MAX_PATH_LEN];
        strncpy(parent_path, path, MAX_PATH_LEN - 1);
        parent_path[MAX_PATH_LEN - 1] = '\0';
        char* last_slash = strrchr(parent_path, '/');
        if (last_slash == parent_path)
            parent_path[1] = '\0';
        else if (last_slash)
            *last_slash = '\0';

        int err;
        struct vnode* parent = vfs_resolve_path_locked(parent_path, NULL, &err);
        if (parent)
        {
            root->parent = parent;
        }
    }
    else
    {
        root->parent = NULL;
    }

    strncpy(mount_table[mount_count].path, path, MAX_PATH_LEN);
    mount_table[mount_count].root = root;
    atomic_inc(&root->refcount);
    mount_count++;
    spin_unlock(&vfs_lock);
    return PERS_SUCCESS;
}

int vfs_unmount(const char* path)
{
    if (path == NULL)
        return -PERS_ERR_NOT_FOUND;

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

            vfs_vnode_put(root);
            return PERS_SUCCESS;
        }
    }
    spin_unlock(&vfs_lock);
    return -PERS_ERR_NOT_FOUND;
}

static struct mount_entry* find_mount(const char* path, int* error)
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
    {
        *error = -PERS_ERR_NOT_FOUND;
        return NULL;
    }
    *error = PERS_SUCCESS;
    return &mount_table[longest_match_index];
}

static struct vnode* vfs_resolve_path_locked(const char* path, struct vnode* cwd, int* error)
{
    struct vnode* curr = NULL;
    char filepath[MAX_PATH_LEN];
    const char* path_remainder = path;

    if (path[0] == '/')
    {
        struct mount_entry* best_match = find_mount(path, error);
        if (best_match == NULL)
        {
            return NULL;
        }

        curr = best_match->root;
        atomic_inc(&curr->refcount);
        size_t len = strlen(best_match->path);

        // exact match
        if (strcmp(path, best_match->path) == 0)
        {
            *error = PERS_SUCCESS;
            return curr;
        }

        path_remainder = (char*)path + len;
        if (*path_remainder == '/')
            path_remainder++;
    }
    else
    {
        if (cwd == NULL)
        {
            struct mount_entry* root_match = find_mount("/", error);
            if (root_match == NULL)
            {
                return NULL;
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

    strncpy(filepath, path_remainder, MAX_PATH_LEN - 1);
    filepath[MAX_PATH_LEN - 1] = '\0';

    char* saveptr;
    char* token = strtok_r(filepath, "/", &saveptr);

    while (token != NULL)
    {
        struct vnode* next = NULL;
        if (strcmp(token, ".") == 0)
        {
            next = curr;
            atomic_inc(&next->refcount);
        }
        else if (strcmp(token, "..") == 0)
        {
            if (curr->parent != NULL)
            {
                next = curr->parent;
                atomic_inc(&next->refcount);
            }
            else
            {
                next = curr;
                atomic_inc(&next->refcount);
            }
        }
        else
        {
            if (curr->ops->lookup == NULL)
            {
                vfs_vnode_put(curr);
                *error = -PERS_ERR_NOT_A_DIRECTORY;
                return NULL;
            }

            next = curr->ops->lookup(curr, token);
            if (next == NULL)
            {
                vfs_vnode_put(curr);
                *error = -PERS_ERR_NOT_FOUND;
                return NULL;
            }
        }

        vfs_vnode_put(curr);
        curr = next;

        token = strtok_r(NULL, "/", &saveptr);
    }
    *error = PERS_SUCCESS;
    return curr;
}

struct vnode* vfs_resolve_path(const char* path, struct vnode* cwd, int* error)
{
    spin_lock(&vfs_lock);
    struct vnode* res = vfs_resolve_path_locked(path, cwd, error);
    spin_unlock(&vfs_lock);
    return res;
}

int vfs_open_pid(const char* path, int flags, uint32_t pid)
{
    int error = 0;
    if (pid >= PROCESS_TABLE_SIZE)
        return -PERS_ERR_NO_SUCH_PROCESS;

    struct vnode* node = vfs_resolve_path(path, process_table[pid].cwd, &error);
    if (node == NULL)
        return error;

    spin_lock(&process_table[pid].fd_lock);
    for (size_t i = 0; i < MAX_FDS; i++)
    {
        struct file* f = process_table[pid].fd_table[i];

        if (f && f->node->internal_info == node->internal_info && f->node->ops == node->ops)
        {
            if (node->type == VNODE_TYPE_DEVICE)
                continue;

            spin_unlock(&process_table[pid].fd_lock);
            vfs_vnode_put(node);
            return -PERS_ERR_ALREADY_EXISTS;
        }
    }
    spin_unlock(&process_table[pid].fd_lock);

    struct file* new_file = (struct file*)slab_alloc(sizeof(struct file));
    if (!new_file)
    {
        vfs_vnode_put(node);
        return -PERS_ERR_OUT_OF_MEMORY;
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
        vfs_vnode_put(node);
        slab_free(new_file);
        return -PERS_ERR_OUT_OF_RESOURCES;
    }
    return ok;
}

int vfs_open(const char* path, int flags)
{
    int curr_process_pid = process_find_current();
    if (curr_process_pid < 0)
        return curr_process_pid; // this is the err code
    return vfs_open_pid(path, flags, (uint32_t)curr_process_pid);
}

int vfs_close(int fd)
{
    if (fd < 0 || fd >= MAX_FDS)
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;

    int curr_process_pid = process_find_current();
    if (curr_process_pid < 0 || curr_process_pid >= PROCESS_TABLE_SIZE)
        return curr_process_pid; // error code from process_find_current()

    struct process* p = &process_table[curr_process_pid];
    spin_lock(&p->fd_lock);
    struct file* f = p->fd_table[fd];

    if (f == NULL)
    {
        spin_unlock(&p->fd_lock);
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    p->fd_table[fd] = NULL;
    spin_unlock(&p->fd_lock);

    if (atomic_dec_and_test(&f->refcount))
    {
        vfs_vnode_put(f->node);
        slab_free(f);
    }

    return PERS_SUCCESS;
}

off_t vfs_lseek(int fd, off_t offset, int whence)
{
    if (fd < 0 || fd >= MAX_FDS)
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;

    int curr_process_pid = process_find_current();
    if (curr_process_pid < 0 || curr_process_pid >= PROCESS_TABLE_SIZE)
        return curr_process_pid; // error code from process....

    struct process* p = &process_table[curr_process_pid];
    spin_lock(&p->fd_lock);
    struct file* f = p->fd_table[fd];
    if (f == NULL)
    {
        spin_unlock(&p->fd_lock);
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
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
        return -PERS_ERR_NOT_IMPLEMENTED;
    }

    if (new_offset < 0)
    {
        spin_unlock(&p->fd_lock);
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    f->offset = new_offset;
    off_t res = f->offset;
    spin_unlock(&p->fd_lock);
    return res;
}

int vfs_read(int fd, void* buffer, size_t count)
{
    if (fd < 0 || fd >= MAX_FDS)
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;

    int curr_process_pid = process_find_current();
    if (curr_process_pid < 0 || curr_process_pid >= PROCESS_TABLE_SIZE)
        return curr_process_pid;

    struct process* p = &process_table[curr_process_pid];
    spin_lock(&p->fd_lock);
    struct file* f = p->fd_table[fd];
    if (f == NULL)
    {
        spin_unlock(&p->fd_lock);
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }
    atomic_inc(&f->refcount);
    spin_unlock(&p->fd_lock);

    // check read flags
    int access_mode = f->flags & O_ACCMODE;
    if ((access_mode != O_RDONLY && access_mode != O_RDWR) || f->node->ops->read == NULL)
    {
        atomic_dec_and_test(&f->refcount);
        return -PERS_ERR_PERMISSION_DENIED;
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
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;

    int curr_process_pid = process_find_current();
    if (curr_process_pid < 0 || curr_process_pid >= PROCESS_TABLE_SIZE)
        return curr_process_pid;

    struct process* p = &process_table[curr_process_pid];
    spin_lock(&p->fd_lock);
    struct file* f = p->fd_table[fd];
    if (f == NULL)
    {
        spin_unlock(&p->fd_lock);
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }
    atomic_inc(&f->refcount);
    spin_unlock(&p->fd_lock);

    // check write flags
    int access_mode = f->flags & O_ACCMODE;
    if ((access_mode != O_WRONLY && access_mode != O_RDWR) || f->node->ops->write == NULL)
    {
        atomic_dec_and_test(&f->refcount);
        return -PERS_ERR_PERMISSION_DENIED;
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
