/*
 * vfs.c - Implementation of the Virtual Filesystem (VFS) layer.
 *
 * This module provides the infrastructure for mounting multiple filesystems
 * into a single unified hierarchy and dispatching calls to drivers.
 *
 * Lock Ordering:
 *   vfs_lock -> process_table_lock -> process.fd_lock
 *
 * Critical: vfs_lock is only held briefly to access the mount table.
 * Filesystem callbacks (lookup, readdir, etc.) are called WITHOUT holding vfs_lock
 * to allow them to acquire process locks if needed.
 */

#include "fs/vfs.h"

#include "stdio.h"
#include "string.h"

#include "uapi/errors.h"

#include "core/lock.h"
#include "mm/heap.h"
#include "mm/slab.h"
#include "fs/devfs.h"
#include "fs/pagecache.h"
#include "sched/process.h"

/*
 * struct vfs_mount_entry - Internal registration of a mounted filesystem.
 */
struct vfs_mount_entry {
    char path[VFS_MAX_PATH_LEN];
    struct vfs_vnode *root;
};

static struct vfs_mount_entry vfs_mount_table[VFS_MAX_MOUNTS];
static int vfs_mount_count = 0;
static spinlock_t vfs_lock = SPINLOCK_INIT;

/*
 * find_mount - Performs a longest-prefix match to find the mount point for a path.
 */
static struct vfs_mount_entry *find_mount(const char *path, int *error)
{
    int longest_match_index = -1;
    size_t longest_match_len = 0;

    for (int i = 0; i < vfs_mount_count; i++) {
        size_t len = strlen(vfs_mount_table[i].path);

        if (strncmp(vfs_mount_table[i].path, path, len) == 0) {
            if (len > 1 && path[len] != '\0' && path[len] != '/') {
                continue;
            }

            if (len >= longest_match_len) {
                longest_match_len = len;
                longest_match_index = i;
            }
        }
    }

    if (longest_match_index == -1) {
        *error = -PERS_ERR_NOT_FOUND;
        return NULL;
    }

    *error = PERS_SUCCESS;
    return &vfs_mount_table[longest_match_index];
}

/*
 * vfs_resolve_path_locked - Core traversal logic (internal, assumes lock held).
 */
static struct vfs_vnode *vfs_resolve_path_locked(const char *path, struct vfs_vnode *cwd,
                                                 int *error)
{
    struct vfs_vnode *curr = NULL;
    char filepath[VFS_MAX_PATH_LEN];
    const char *path_remainder = path;

    if (path[0] == '/') {
        struct vfs_mount_entry *best_match = find_mount(path, error);
        if (!best_match) {
            return NULL;
        }

        curr = best_match->root;
        atomic_inc(&curr->refcount);
        size_t len = strlen(best_match->path);

        if (strcmp(path, best_match->path) == 0) {
            *error = PERS_SUCCESS;
            return curr;
        }

        path_remainder = path + len;
        if (*path_remainder == '/') {
            path_remainder++;
        }
    } else {
        if (!cwd) {
            struct vfs_mount_entry *root_match = find_mount("/", error);
            if (!root_match) {
                return NULL;
            }
            curr = root_match->root;
        } else {
            curr = cwd;
        }
        atomic_inc(&curr->refcount);
    }

    if (*path_remainder == '\0') {
        *error = PERS_SUCCESS;
        return curr;
    }

    strncpy(filepath, path_remainder, VFS_MAX_PATH_LEN - 1);
    filepath[VFS_MAX_PATH_LEN - 1] = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(filepath, "/", &saveptr);

    while (token) {
        struct vfs_vnode *next = NULL;
        if (strcmp(token, ".") == 0) {
            next = curr;
            atomic_inc(&next->refcount);
        } else if (strcmp(token, "..") == 0) {
            next = curr->parent ? curr->parent : curr;
            atomic_inc(&next->refcount);
        } else {
            struct vfs_vnode *mount_node = NULL;
            for (size_t i = 1; i < (size_t)vfs_mount_count; i++) {
                if (vfs_mount_table[i].root && vfs_mount_table[i].root->parent == curr
                    && strcmp(vfs_mount_table[i].root->name, token) == 0) {
                    mount_node = vfs_mount_table[i].root;
                    break;
                }
            }

            if (mount_node) {
                next = mount_node;
                atomic_inc(&next->refcount);
            } else {
                if (!curr->ops || !curr->ops->lookup) {
                    vfs_vnode_put(curr);
                    *error = -PERS_ERR_NOT_A_DIRECTORY;
                    return NULL;
                }

                next = curr->ops->lookup(curr, token);
                if (!next) {
                    vfs_vnode_put(curr);
                    *error = -PERS_ERR_NOT_FOUND;
                    return NULL;
                }
                strncpy(next->name, token, sizeof(next->name) - 1);
                next->name[sizeof(next->name) - 1] = '\0';
            }
        }

        vfs_vnode_put(curr);
        curr = next;
        token = strtok_r(NULL, "/", &saveptr);
    }

    *error = PERS_SUCCESS;
    return curr;
}

/*
 * vfs_vnode_stat - Helper to fill metadata from a generic vnode.
 */
static int vfs_vnode_stat(struct vfs_vnode *node, struct stat *buf)
{
    if (!node || !buf) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    memset(buf, 0, sizeof(struct stat));

    if (node->type == VFS_VNODE_TYPE_DIR) {
        buf->st_mode = S_IFDIR | 0755;
    } else if (node->type == VFS_VNODE_TYPE_DEVICE) {
        buf->st_mode = S_IFCHR | 0666;
    } else {
        buf->st_mode = S_IFREG | 0644;
    }

    buf->st_size = (uint64_t)node->file_size;
    buf->st_nlink = 1;
    buf->st_uid = 0;
    buf->st_gid = 0;

    if (node->ops && node->ops->stat) {
        return node->ops->stat(node, buf);
    }

    return PERS_SUCCESS;
}

/*
 * vfs_init - Initializes internal mount infrastructure.
 */
void vfs_init(void)
{
    unsigned long flags = spin_lock_irqsave(&vfs_lock);
    vfs_mount_count = 0;
    for (size_t i = 0; i < VFS_MAX_MOUNTS; i++) {
        memset(vfs_mount_table[i].path, 0, VFS_MAX_PATH_LEN);
        vfs_mount_table[i].root = NULL;
    }
    spin_unlock_irqrestore(&vfs_lock, flags);

    pr_info("vfs: Virtual File System initialized\n");
}

#ifdef CONFIG_TESTS
static atomic_t vfs_live_files = ATOMIC_INIT(0);

unsigned long vfs_test_live_files(void)
{
    return (unsigned long)vfs_live_files.counter;
}

struct vfs_file *vfs_test_file_at(int fd)
{
    int pid = process_find_current();
    if (fd < 0 || fd >= VFS_MAX_FDS || pid < 0) {
        return NULL;
    }

    struct process *p = &process_table[pid];
    unsigned long fdflags = spin_lock_irqsave(&p->fd_lock);
    struct vfs_file *f = p->fd_table[fd];
    spin_unlock_irqrestore(&p->fd_lock, fdflags);
    return f;
}
#endif

/*
 * vfs_file_alloc - Allocates an open-file object holding one reference.
 */
struct vfs_file *vfs_file_alloc(void)
{
    struct vfs_file *f = (struct vfs_file *)slab_alloc(sizeof(struct vfs_file));
    if (!f) {
        return NULL;
    }

    memset(f, 0, sizeof(*f));
    f->refcount.counter = 1;

#ifdef CONFIG_TESTS
    atomic_inc(&vfs_live_files);
#endif
    return f;
}

/*
 * vfs_file_put - Drops a reference, releasing the file with the last one.
 *
 * Every release goes through here. The I/O paths take a reference for the
 * duration of the call, so whichever of them and close() finishes last is the
 * one that must run the driver's close and free the object.
 */
void vfs_file_put(struct vfs_file *f)
{
    if (!f) {
        return;
    }

    if (!atomic_dec_and_test(&f->refcount)) {
        return;
    }

    if (f->node) {
        if (f->node->ops && f->node->ops->close) {
            f->node->ops->close(f);
        }
        vfs_vnode_put(f->node);
    }

#ifdef CONFIG_TESTS
    atomic_dec_and_test(&vfs_live_files);
#endif
    slab_free(f);
}

/*
 * vfs_vnode_put - Reference counting entry point for vnodes.
 */
void vfs_vnode_put(struct vfs_vnode *node)
{
    if (!node) {
        return;
    }

    if (atomic_dec_and_test(&node->refcount)) {
        struct vfs_vnode *parent = node->parent;
        slab_free(node);

        if (parent) {
            vfs_vnode_put(parent);
        }
    }
}

/*
 * vfs_mount - Registers a new filesystem root at a specific path.
 */
int vfs_mount(const char *path, struct vfs_vnode *root)
{
    if (!path || path[0] != '/' || !root) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    unsigned long flags = spin_lock_irqsave(&vfs_lock);

    if (vfs_mount_count >= VFS_MAX_MOUNTS) {
        spin_unlock_irqrestore(&vfs_lock, flags);
        return -PERS_ERR_OUT_OF_RESOURCES;
    }

    for (int i = 0; i < vfs_mount_count; i++) {
        if (strcmp(path, vfs_mount_table[i].path) == 0) {
            spin_unlock_irqrestore(&vfs_lock, flags);
            return -PERS_ERR_ALREADY_EXISTS;
        }
    }

    if (strcmp(path, "/") != 0) {
        char parent_path[VFS_MAX_PATH_LEN];
        strncpy(parent_path, path, VFS_MAX_PATH_LEN - 1);
        parent_path[VFS_MAX_PATH_LEN - 1] = '\0';

        char *last_slash = strrchr(parent_path, '/');
        if (last_slash == parent_path) {
            parent_path[1] = '\0';
        } else if (last_slash) {
            *last_slash = '\0';
        }

        char *orig_last_slash = strrchr(path, '/');
        if (orig_last_slash && orig_last_slash[1] != '\0') {
            strncpy(root->name, orig_last_slash + 1, sizeof(root->name) - 1);
            root->name[sizeof(root->name) - 1] = '\0';
        }

        int err;
        struct vfs_vnode *parent = vfs_resolve_path_locked(parent_path, NULL, &err);
        if (parent) {
            root->parent = parent;
        }
    } else {
        root->parent = NULL;
        root->name[0] = '\0';
    }

    strncpy(vfs_mount_table[vfs_mount_count].path, path, VFS_MAX_PATH_LEN);
    vfs_mount_table[vfs_mount_count].root = root;
    atomic_inc(&root->refcount);
    vfs_mount_count++;

    spin_unlock_irqrestore(&vfs_lock, flags);
    return PERS_SUCCESS;
}

/*
 * vfs_unmount - Safely removes a mount entry.
 */
int vfs_unmount(const char *path)
{
    if (!path) {
        return -PERS_ERR_NOT_FOUND;
    }

    unsigned long flags = spin_lock_irqsave(&vfs_lock);
    for (int i = 0; i < vfs_mount_count; i++) {
        if (strcmp(path, vfs_mount_table[i].path) == 0) {
            struct vfs_vnode *root = vfs_mount_table[i].root;

            strncpy(vfs_mount_table[i].path, vfs_mount_table[vfs_mount_count - 1].path,
                    VFS_MAX_PATH_LEN);
            vfs_mount_table[i].root = vfs_mount_table[vfs_mount_count - 1].root;

            memset(vfs_mount_table[vfs_mount_count - 1].path, 0, VFS_MAX_PATH_LEN);
            vfs_mount_table[vfs_mount_count - 1].root = NULL;

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
 * vfs_resolve_path - Thread-safe path resolution from a given CWD.
 *
 * This function only holds vfs_lock when accessing the mount table.
 * Path traversal and filesystem callbacks are done without holding vfs_lock
 * to allow them to acquire process locks if needed.
 */
struct vfs_vnode *vfs_resolve_path(const char *path, struct vfs_vnode *cwd, int *error)
{
    struct vfs_vnode *curr = NULL;
    char filepath[VFS_MAX_PATH_LEN];
    const char *path_remainder = path;

    /* Handle absolute paths - find mount point with vfs_lock */
    if (path[0] == '/') {
        unsigned long flags = spin_lock_irqsave(&vfs_lock);
        struct vfs_mount_entry *best_match = find_mount(path, error);
        if (!best_match) {
            spin_unlock_irqrestore(&vfs_lock, flags);
            return NULL;
        }

        curr = best_match->root;
        atomic_inc(&curr->refcount);
        size_t len = strlen(best_match->path);

        if (strcmp(path, best_match->path) == 0) {
            spin_unlock_irqrestore(&vfs_lock, flags);
            *error = PERS_SUCCESS;
            return curr;
        }

        path_remainder = path + len;
        if (*path_remainder == '/') {
            path_remainder++;
        }
        spin_unlock_irqrestore(&vfs_lock, flags);
    } else {
        /* Relative path - use cwd */
        if (!cwd) {
            unsigned long flags = spin_lock_irqsave(&vfs_lock);
            struct vfs_mount_entry *root_match = find_mount("/", error);
            if (!root_match) {
                spin_unlock_irqrestore(&vfs_lock, flags);
                return NULL;
            }
            curr = root_match->root;
            spin_unlock_irqrestore(&vfs_lock, flags);
        } else {
            curr = cwd;
        }
        atomic_inc(&curr->refcount);
    }

    if (*path_remainder == '\0') {
        *error = PERS_SUCCESS;
        return curr;
    }

    strncpy(filepath, path_remainder, VFS_MAX_PATH_LEN - 1);
    filepath[VFS_MAX_PATH_LEN - 1] = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(filepath, "/", &saveptr);

    while (token) {
        struct vfs_vnode *next = NULL;
        if (strcmp(token, ".") == 0) {
            next = curr;
            atomic_inc(&next->refcount);
        } else if (strcmp(token, "..") == 0) {
            next = curr->parent ? curr->parent : curr;
            atomic_inc(&next->refcount);
        } else {
            struct vfs_vnode *mount_node = NULL;
            unsigned long flags = spin_lock_irqsave(&vfs_lock);
            for (size_t i = 1; i < (size_t)vfs_mount_count; i++) {
                if (vfs_mount_table[i].root && vfs_mount_table[i].root->parent == curr
                    && strcmp(vfs_mount_table[i].root->name, token) == 0) {
                    mount_node = vfs_mount_table[i].root;
                    atomic_inc(&mount_node->refcount);
                    break;
                }
            }
            spin_unlock_irqrestore(&vfs_lock, flags);

            if (mount_node) {
                next = mount_node;
            } else {
                if (!curr->ops || !curr->ops->lookup) {
                    vfs_vnode_put(curr);
                    *error = -PERS_ERR_NOT_A_DIRECTORY;
                    return NULL;
                }

                next = curr->ops->lookup(curr, token);
                if (!next) {
                    vfs_vnode_put(curr);
                    *error = -PERS_ERR_NOT_FOUND;
                    return NULL;
                }
                strncpy(next->name, token, sizeof(next->name) - 1);
                next->name[sizeof(next->name) - 1] = '\0';
            }
        }

        vfs_vnode_put(curr);
        curr = next;
        token = strtok_r(NULL, "/", &saveptr);
    }

    *error = PERS_SUCCESS;
    return curr;
}

/*
 * vfs_open_pid - Opens a file for a specific process ID.
 */
int vfs_open_pid(const char *path, int flags, uint32_t pid)
{
    int error = 0;
    if (pid >= PROCESS_TABLE_SIZE) {
        return -PERS_ERR_NO_SUCH_PROCESS;
    }

    struct vfs_vnode *node = vfs_resolve_path(path, process_table[pid].cwd, &error);
    if (!node) {
        if (!(flags & VFS_O_CREAT)) /* not asking to create → just fail */
            return error;

        /* Split path into parent + name, resolve parent, call create */
        char kpath[VFS_MAX_PATH_LEN];
        strncpy(kpath, path, VFS_MAX_PATH_LEN - 1);
        kpath[VFS_MAX_PATH_LEN - 1] = '\0';

        char *slash = strrchr(kpath, '/');
        const char *parent_path, *name;
        if (!slash) {
            parent_path = ".";
            name = kpath;
        } else if (slash == kpath) {
            parent_path = "/";
            name = slash + 1;
        } else {
            *slash = '\0';
            parent_path = kpath;
            name = slash + 1;
        }

        struct vfs_vnode *parent = vfs_resolve_path(parent_path, process_table[pid].cwd, &error);
        if (!parent)
            return error;

        if (!parent->ops || !parent->ops->create) {
            vfs_vnode_put(parent);
            return -PERS_ERR_OPERATION_NOT_SUPPORTED;
        }

        int cres = parent->ops->create(parent, name);
        vfs_vnode_put(parent);
        if (cres != PERS_SUCCESS)
            return cres;

        /* Now resolve the newly-created file */
        node = vfs_resolve_path(path, process_table[pid].cwd, &error);
        if (!node)
            return error;
    }

    unsigned long fdflags = spin_lock_irqsave(&process_table[pid].fd_lock);
    for (size_t i = 0; i < VFS_MAX_FDS; i++) {
        struct vfs_file *f = process_table[pid].fd_table[i];
        if (f && f->node->internal_info == node->internal_info && f->node->ops == node->ops) {
            if (node->type != VFS_VNODE_TYPE_DEVICE) {
                spin_unlock_irqrestore(&process_table[pid].fd_lock, fdflags);
                vfs_vnode_put(node);
                return -PERS_ERR_ALREADY_EXISTS;
            }
        }
    }
    spin_unlock_irqrestore(&process_table[pid].fd_lock, fdflags);

    struct vfs_file *new_file = vfs_file_alloc();
    if (!new_file) {
        vfs_vnode_put(node);
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    new_file->node = node;
    new_file->flags = flags;

    int slot = -1;
    fdflags = spin_lock_irqsave(&process_table[pid].fd_lock);
    for (size_t i = 0; i < VFS_MAX_FDS; i++) {
        if (!process_table[pid].fd_table[i]) {
            process_table[pid].fd_table[i] = new_file;
            process_table[pid].fd_flags[i] = (flags & VFS_O_CLOEXEC) ? VFS_FD_CLOEXEC : 0;
            slot = (int)i;
            break;
        }
    }
    spin_unlock_irqrestore(&process_table[pid].fd_lock, fdflags);

    if (slot == -1) {
        /* Releasing the file drops its vnode reference too. */
        vfs_file_put(new_file);
        return -PERS_ERR_OUT_OF_RESOURCES;
    }
    return slot;
}

/*
 * vfs_open - Standard process-relative file open.
 */
int vfs_open(const char *path, int flags)
{
    int pid = process_find_current();
    if (pid < 0) {
        return pid;
    }
    return vfs_open_pid(path, flags, (uint32_t)pid);
}

/*
 * vfs_close - Decrements file object refcount and destroys if zero.
 */
int vfs_close(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FDS) {
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    int pid = process_find_current();
    if (pid < 0) {
        return pid;
    }

    struct process *p = &process_table[pid];
    unsigned long fdflags = spin_lock_irqsave(&p->fd_lock);
    struct vfs_file *f = p->fd_table[fd];

    if (!f) {
        spin_unlock_irqrestore(&p->fd_lock, fdflags);
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    p->fd_table[fd] = NULL;
    spin_unlock_irqrestore(&p->fd_lock, fdflags);

    vfs_file_put(f);
    return PERS_SUCCESS;
}

/*
 * vfs_lseek - Updates the current read/write offset.
 */
vfs_off_t vfs_lseek(int fd, vfs_off_t offset, int whence)
{
    if (fd < 0 || fd >= VFS_MAX_FDS) {
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    int pid = process_find_current();
    if (pid < 0) {
        return pid;
    }

    struct process *p = &process_table[pid];
    unsigned long fdflags = spin_lock_irqsave(&p->fd_lock);
    struct vfs_file *f = p->fd_table[fd];
    if (!f) {
        spin_unlock_irqrestore(&p->fd_lock, fdflags);
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    vfs_off_t new_offset;
    switch (whence) {
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
            spin_unlock_irqrestore(&p->fd_lock, fdflags);
            return -PERS_ERR_NOT_IMPLEMENTED;
    }

    if (new_offset < 0) {
        spin_unlock_irqrestore(&p->fd_lock, fdflags);
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    f->offset = new_offset;
    spin_unlock_irqrestore(&p->fd_lock, fdflags);
    return new_offset;
}

/*
 * vfs_read - Dispatches read request to the underlying vnode driver.
 */
int vfs_read(int fd, void *buffer, size_t count)
{
    if (fd < 0 || fd >= VFS_MAX_FDS) {
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    int pid = process_find_current();
    if (pid < 0) {
        return pid;
    }

    struct process *p = &process_table[pid];
    unsigned long fdflags = spin_lock_irqsave(&p->fd_lock);
    struct vfs_file *f = p->fd_table[fd];
    if (!f) {
        spin_unlock_irqrestore(&p->fd_lock, fdflags);
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }
    atomic_inc(&f->refcount);
    spin_unlock_irqrestore(&p->fd_lock, fdflags);

    int mode = f->flags & VFS_O_ACCMODE;
    if ((mode != VFS_O_RDONLY && mode != VFS_O_RDWR) || !f->node->ops->read) {
        vfs_file_put(f);
        return -PERS_ERR_PERMISSION_DENIED;
    }

    int bytes = f->node->ops->read(f, buffer, count);
    vfs_file_put(f);
    return bytes;
}

/*
 * vfs_pread - Dispatches pread request to the underlying vnode driver with an offset.
 */
int vfs_pread(int fd, void *buffer, size_t count, vfs_off_t offset)
{
    if (fd < 0 || fd >= VFS_MAX_FDS) {
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    if (offset < 0) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    int pid = process_find_current();
    if (pid < 0) {
        return pid;
    }

    struct process *p = &process_table[pid];
    unsigned long fdflags = spin_lock_irqsave(&p->fd_lock);
    struct vfs_file *f = p->fd_table[fd];
    if (!f) {
        spin_unlock_irqrestore(&p->fd_lock, fdflags);
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }
    atomic_inc(&f->refcount);
    spin_unlock_irqrestore(&p->fd_lock, fdflags);

    int mode = f->flags & VFS_O_ACCMODE;
    if ((mode != VFS_O_RDONLY && mode != VFS_O_RDWR) || !f->node->ops->read) {
        vfs_file_put(f);
        return -PERS_ERR_PERMISSION_DENIED;
    }

    struct vfs_file temp_f;
    temp_f.node = f->node;
    temp_f.offset = offset;
    temp_f.flags = f->flags;

    int bytes = temp_f.node->ops->read(&temp_f, buffer, count);
    vfs_file_put(f);
    return bytes;
}

/*
 * vfs_readdir - Reads raw driver entries and appends VFS mount points.
 */
int vfs_readdir(int fd, void *buffer, size_t count)
{
    if (fd < 0 || fd >= VFS_MAX_FDS) {
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    /* Entries are written whole; a partial buffer would also underflow the
     * remaining-space arithmetic below. */
    if (count < sizeof(struct vfs_dirent)) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    int pid = process_find_current();
    if (pid < 0) {
        return pid;
    }

    struct process *p = &process_table[pid];
    unsigned long fdflags = spin_lock_irqsave(&p->fd_lock);
    struct vfs_file *f = p->fd_table[fd];
    if (!f) {
        spin_unlock_irqrestore(&p->fd_lock, fdflags);
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }
    atomic_inc(&f->refcount);
    spin_unlock_irqrestore(&p->fd_lock, fdflags);

    if (f->node->type != VFS_VNODE_TYPE_DIR || !f->node->ops->readdir) {
        vfs_file_put(f);
        return -PERS_ERR_NOT_A_DIRECTORY;
    }

    size_t dirent_size = sizeof(struct vfs_dirent);
    size_t max_entries = count / dirent_size;
    struct vfs_dirent *dirents = (struct vfs_dirent *)buffer;

    vfs_off_t orig_offset = f->offset;
    /* Offset layout:
     * [63:34] - Mount index
     * [33:32] - Dot/DotDot index (0: none, 1: ".", 2: "..")
     * [31: 0] - FS-specific offset
     */
    uint32_t mount_idx = (uint32_t)((orig_offset >> 34) & 0x3FFFFFFF);
    uint32_t dot_idx = (uint32_t)((orig_offset >> 32) & 0x03);
    f->offset = orig_offset & 0xFFFFFFFF;

    int res = 0;

    /* 1. Handle "." and ".." */
    if (dot_idx < 2) {
        if ((size_t)res < max_entries && dot_idx == 0) {
            strncpy(dirents[res].name, ".", 255);
            dirents[res].name[255] = '\0';
            dirents[res].ino = 1;
            res++;
            dot_idx = 1;
        }

        if ((size_t)res < max_entries && dot_idx == 1) {
            strncpy(dirents[res].name, "..", 255);
            dirents[res].name[255] = '\0';
            dirents[res].ino = 2;
            res++;
            dot_idx = 2;
        }

        if ((size_t)res >= max_entries) {
            goto readdir_done;
        }
    }

    /* 2. Handle Filesystem-specific entries */
    if (mount_idx == 0) {
        int fs_res = f->node->ops->readdir(f, buffer + res * dirent_size,
                                           (max_entries - (size_t)res) * dirent_size);
        if (fs_res < 0) {
            f->offset = orig_offset;
            vfs_file_put(f);
            return fs_res;
        }
        res += fs_res;

        if ((size_t)res == max_entries) {
            goto readdir_done;
        }
        mount_idx = 1;
    }

    /* 3. Handle VFS Mount points */
    unsigned long flags = spin_lock_irqsave(&vfs_lock);
    while ((size_t)res < max_entries && (mount_idx - 1) < (uint32_t)vfs_mount_count) {
        int i = mount_idx - 1;
        if (vfs_mount_table[i].root) {
            int is_child = 0;

            if (vfs_mount_table[i].root->parent == f->node) {
                is_child = 1;
            } else if (f->node->parent == NULL && f->node->name[0] == '\0') {
                const char *mpath = vfs_mount_table[i].path;
                if (mpath[0] == '/' && mpath[1] != '\0' && strchr(mpath + 1, '/') == NULL) {
                    is_child = 1;
                }
            }

            if (is_child) {
                strncpy(dirents[res].name, vfs_mount_table[i].root->name, 255);
                dirents[res].name[255] = '\0';
                dirents[res].ino = 9000 + (uint32_t)i;
                res++;
            }
        }
        mount_idx++;
    }
    spin_unlock_irqrestore(&vfs_lock, flags);

readdir_done:
    f->offset =
        (f->offset & 0xFFFFFFFF) | ((vfs_off_t)dot_idx << 32) | ((vfs_off_t)mount_idx << 34);

    vfs_file_put(f);
    return res;
}

/*
 * vfs_write - Dispatches write request to the underlying vnode driver.
 */
int vfs_write(int fd, const void *buffer, size_t count)
{
    if (fd < 0 || fd >= VFS_MAX_FDS) {
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    int pid = process_find_current();
    if (pid < 0) {
        return pid;
    }

    struct process *p = &process_table[pid];
    unsigned long fdflags = spin_lock_irqsave(&p->fd_lock);
    struct vfs_file *f = p->fd_table[fd];
    if (!f) {
        spin_unlock_irqrestore(&p->fd_lock, fdflags);
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }
    atomic_inc(&f->refcount);
    spin_unlock_irqrestore(&p->fd_lock, fdflags);

    int mode = f->flags & VFS_O_ACCMODE;
    if ((mode != VFS_O_WRONLY && mode != VFS_O_RDWR) || !f->node->ops->write) {
        vfs_file_put(f);
        return -PERS_ERR_PERMISSION_DENIED;
    }

    if (f->flags & VFS_O_APPEND) {
        f->offset = f->node->file_size;
    }

    int bytes = f->node->ops->write(f, buffer, count);
    vfs_file_put(f);
    return bytes;
}

/*
 * vfs_pwrite - Dispatches write request to the underlying vnode driver with an offset.
 */
int vfs_pwrite(int fd, const void *buffer, size_t count, vfs_off_t offset)
{
    if (fd < 0 || fd >= VFS_MAX_FDS) {
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    if (offset < 0) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    int pid = process_find_current();
    if (pid < 0) {
        return pid;
    }

    struct process *p = &process_table[pid];
    unsigned long fdflags = spin_lock_irqsave(&p->fd_lock);
    struct vfs_file *f = p->fd_table[fd];
    if (!f) {
        spin_unlock_irqrestore(&p->fd_lock, fdflags);
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }
    atomic_inc(&f->refcount);
    spin_unlock_irqrestore(&p->fd_lock, fdflags);

    int mode = f->flags & VFS_O_ACCMODE;
    if ((mode != VFS_O_WRONLY && mode != VFS_O_RDWR) || !f->node->ops->write) {
        vfs_file_put(f);
        return -PERS_ERR_PERMISSION_DENIED;
    }

    struct vfs_file temp_f;
    temp_f.node = f->node;
    temp_f.flags = f->flags;

    if (f->flags & VFS_O_APPEND) {
        temp_f.offset = f->node->file_size;
    } else {
        temp_f.offset = offset;
    }

    int bytes = f->node->ops->write(&temp_f, buffer, count);
    vfs_file_put(f);
    return bytes;
}

/*
 * vfs_stat - Path-based metadata retrieval entry point.
 */
int vfs_stat(const char *path, struct stat *buf)
{
    int pid_idx = process_find_current();
    if (pid_idx < 0) {
        return pid_idx;
    }

    int error = 0;
    struct vfs_vnode *node = vfs_resolve_path(path, process_table[pid_idx].cwd, &error);
    if (!node) {
        return error;
    }

    int res = vfs_vnode_stat(node, buf);
    vfs_vnode_put(node);
    return res;
}

/*
 * vfs_dup2 - Replaces a descriptor slot with a copy of another.
 */
int vfs_dup2(int oldfd, int newfd)
{
    if (oldfd < 0 || oldfd >= VFS_MAX_FDS || newfd < 0 || newfd >= VFS_MAX_FDS) {
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    int pid = process_find_current();
    if (pid < 0) {
        return pid;
    }

    struct process *p = &process_table[pid];
    struct vfs_file *to_free = NULL;

    unsigned long fdflags = spin_lock_irqsave(&p->fd_lock);
    struct vfs_file *old_f = p->fd_table[oldfd];
    if (!old_f) {
        spin_unlock_irqrestore(&p->fd_lock, fdflags);
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    if (oldfd == newfd) {
        spin_unlock_irqrestore(&p->fd_lock, fdflags);
        return newfd;
    }

    to_free = p->fd_table[newfd];
    p->fd_table[newfd] = old_f;
    p->fd_flags[newfd] = 0; // POSIX specifies dup2 clears FD_CLOEXEC
    atomic_inc(&old_f->refcount);
    spin_unlock_irqrestore(&p->fd_lock, fdflags);

    vfs_file_put(to_free);
    return newfd;
}

/*
 * vfs_chdir - Migrates the current process working directory.
 */
int vfs_chdir(const char *kpath)
{
    int pid = process_find_current();
    if (pid < 0) {
        return pid;
    }

    struct process *p = &process_table[pid];
    int error = 0;

    struct vfs_vnode *node = vfs_resolve_path(kpath, p->cwd, &error);
    if (!node) {
        return error;
    }

    if (node->type != VFS_VNODE_TYPE_DIR) {
        vfs_vnode_put(node);
        return -PERS_ERR_NOT_A_DIRECTORY;
    }

    unsigned long fdflags = spin_lock_irqsave(&p->fd_lock);
    if (p->cwd) {
        vfs_vnode_put(p->cwd);
    }
    p->cwd = node;
    spin_unlock_irqrestore(&p->fd_lock, fdflags);

    return PERS_SUCCESS;
}

/*
 * vfs_getcwd - Reconstructs the absolute path of the current CWD.
 */
int vfs_getcwd(char *buf, size_t size)
{
    if (!buf || size == 0) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    int pid = process_find_current();
    if (pid < 0) {
        return pid;
    }

    struct process *p = &process_table[pid];
    unsigned long fdflags = spin_lock_irqsave(&p->fd_lock);
    struct vfs_vnode *curr_node = p->cwd;
    if (!curr_node) {
        spin_unlock_irqrestore(&p->fd_lock, fdflags);
        return -PERS_ERR_NOT_FOUND;
    }
    atomic_inc(&curr_node->refcount);
    spin_unlock_irqrestore(&p->fd_lock, fdflags);

    char temp_path[VFS_MAX_PATH_LEN];
    char *ptr = temp_path + VFS_MAX_PATH_LEN - 1;
    *ptr = '\0';

    struct vfs_vnode *node = curr_node;

    if (!node->parent) {
        if (size < 2) {
            vfs_vnode_put(curr_node);
            return -PERS_ERR_INVALID_ARGUMENT;
        }
        buf[0] = '/';
        buf[1] = '\0';
        vfs_vnode_put(curr_node);
        return 0;
    }

    while (node && node->parent) {
        size_t len = strlen(node->name);
        if (ptr - temp_path < (ptrdiff_t)len + 1) {
            vfs_vnode_put(curr_node);
            return -PERS_ERR_INVALID_ARGUMENT;
        }
        ptr -= len;
        memcpy(ptr, node->name, len);

        ptr--;
        *ptr = '/';

        node = node->parent;
    }

    size_t result_len = (temp_path + VFS_MAX_PATH_LEN - 1) - ptr;
    if (result_len >= size) {
        vfs_vnode_put(curr_node);
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    memcpy(buf, ptr, result_len + 1);
    vfs_vnode_put(curr_node);

    return PERS_SUCCESS;
}

/*
 * vfs_mkdir - Creates a new directory from a given path.
 */
int vfs_mkdir(const char *path)
{
    char kpath[VFS_MAX_PATH_LEN];
    strncpy(kpath, path, VFS_MAX_PATH_LEN);
    kpath[VFS_MAX_PATH_LEN - 1] = '\0';

    char *slash = strrchr(kpath, '/');
    const char *parent_path;
    const char *name;

    if (!slash) {
        parent_path = ".";
        name = kpath;
    } else if (slash == kpath) {
        parent_path = "/";
        name = slash + 1;
    } else {
        *slash = '\0';
        parent_path = kpath;
        name = slash + 1;
    }

    int pid = process_find_current();
    if (pid < 0) {
        return pid;
    }

    struct process *p = &process_table[pid];
    int error = 0;

    struct vfs_vnode *parent = vfs_resolve_path(parent_path, p->cwd, &error);
    if (!parent) {
        return error;
    }

    if (parent->type != VFS_VNODE_TYPE_DIR) {
        vfs_vnode_put(parent);
        return -PERS_ERR_NOT_A_DIRECTORY;
    }

    if (!parent->ops || !parent->ops->mkdir) {
        vfs_vnode_put(parent);
        return -PERS_ERR_OPERATION_NOT_SUPPORTED;
    }

    int res = parent->ops->mkdir(parent, name);
    vfs_vnode_put(parent);
    return res;
}

/*
 * vfs_rmdir - Removes an existing directory from a given path.
 */
int vfs_rmdir(const char *path)
{
    char kpath[VFS_MAX_PATH_LEN];
    strncpy(kpath, path, VFS_MAX_PATH_LEN);
    kpath[VFS_MAX_PATH_LEN - 1] = '\0';

    char *slash = strrchr(kpath, '/');
    const char *parent_path;
    const char *name;

    if (!slash) {
        parent_path = ".";
        name = kpath;
    } else if (slash == kpath) {
        parent_path = "/";
        name = slash + 1;
    } else {
        *slash = '\0';
        parent_path = kpath;
        name = slash + 1;
    }

    int pid = process_find_current();
    if (pid < 0) {
        return pid;
    }

    struct process *p = &process_table[pid];
    int error = 0;

    struct vfs_vnode *parent = vfs_resolve_path(parent_path, p->cwd, &error);
    if (!parent) {
        return error;
    }

    if (parent->type != VFS_VNODE_TYPE_DIR) {
        vfs_vnode_put(parent);
        return -PERS_ERR_NOT_A_DIRECTORY;
    }

    if (!parent->ops || !parent->ops->rmdir) {
        vfs_vnode_put(parent);
        return -PERS_ERR_OPERATION_NOT_SUPPORTED;
    }

    int res = parent->ops->rmdir(parent, name);
    vfs_vnode_put(parent);
    return res;
}

/*
 * vfs_unlink - Unlinks a file from a given path.
 */
int vfs_unlink(const char *path)
{
    char kpath[VFS_MAX_PATH_LEN];
    strncpy(kpath, path, VFS_MAX_PATH_LEN);
    kpath[VFS_MAX_PATH_LEN - 1] = '\0';

    char *slash = strrchr(kpath, '/');
    const char *parent_path;
    const char *name;

    if (!slash) {
        parent_path = ".";
        name = kpath;
    } else if (slash == kpath) {
        parent_path = "/";
        name = slash + 1;
    } else {
        *slash = '\0';
        parent_path = kpath;
        name = slash + 1;
    }

    int pid = process_find_current();
    if (pid < 0) {
        return pid;
    }

    struct process *p = &process_table[pid];
    int error = 0;

    struct vfs_vnode *parent = vfs_resolve_path(parent_path, p->cwd, &error);
    if (!parent) {
        return error;
    }

    if (parent->type != VFS_VNODE_TYPE_DIR) {
        vfs_vnode_put(parent);
        return -PERS_ERR_NOT_A_DIRECTORY;
    }

    if (!parent->ops || !parent->ops->unlink) {
        vfs_vnode_put(parent);
        return -PERS_ERR_OPERATION_NOT_SUPPORTED;
    }

    int res = parent->ops->unlink(parent, name);
    vfs_vnode_put(parent);
    return res;
}

/*
 * vfs_rename - Renames or moves a file or directory.
 */
int vfs_rename(const char *oldpath, const char *newpath)
{
    char kold[VFS_MAX_PATH_LEN];
    strncpy(kold, oldpath, VFS_MAX_PATH_LEN);
    kold[VFS_MAX_PATH_LEN - 1] = '\0';

    char *slash_old = strrchr(kold, '/');
    const char *parent_old;
    const char *name_old;

    if (!slash_old) {
        parent_old = ".";
        name_old = kold;
    } else if (slash_old == kold) {
        parent_old = "/";
        name_old = slash_old + 1;
    } else {
        *slash_old = '\0';
        parent_old = kold;
        name_old = slash_old + 1;
    }

    char knew[VFS_MAX_PATH_LEN];
    strncpy(knew, newpath, VFS_MAX_PATH_LEN);
    knew[VFS_MAX_PATH_LEN - 1] = '\0';

    char *slash_new = strrchr(knew, '/');
    const char *parent_new;
    const char *name_new;

    if (!slash_new) {
        parent_new = ".";
        name_new = knew;
    } else if (slash_new == knew) {
        parent_new = "/";
        name_new = slash_new + 1;
    } else {
        *slash_new = '\0';
        parent_new = knew;
        name_new = slash_new + 1;
    }

    int pid = process_find_current();
    if (pid < 0) {
        return pid;
    }

    struct process *p = &process_table[pid];
    int error = 0;

    struct vfs_vnode *old_parent_node = vfs_resolve_path(parent_old, p->cwd, &error);
    if (!old_parent_node) {
        return error;
    }

    if (old_parent_node->type != VFS_VNODE_TYPE_DIR) {
        vfs_vnode_put(old_parent_node);
        return -PERS_ERR_NOT_A_DIRECTORY;
    }

    struct vfs_vnode *new_parent_node = vfs_resolve_path(parent_new, p->cwd, &error);
    if (!new_parent_node) {
        vfs_vnode_put(old_parent_node);
        return error;
    }

    if (new_parent_node->type != VFS_VNODE_TYPE_DIR) {
        vfs_vnode_put(new_parent_node);
        vfs_vnode_put(old_parent_node);
        return -PERS_ERR_NOT_A_DIRECTORY;
    }

    if (!old_parent_node->ops || !old_parent_node->ops->rename) {
        vfs_vnode_put(new_parent_node);
        vfs_vnode_put(old_parent_node);
        return -PERS_ERR_OPERATION_NOT_SUPPORTED;
    }

    int res = old_parent_node->ops->rename(old_parent_node, name_old, new_parent_node, name_new);

    vfs_vnode_put(new_parent_node);
    vfs_vnode_put(old_parent_node);

    return res;
}

/*
 * vfs_fsync - Flushes all dirty cached pages for a single file descriptor to disk.
 */
int vfs_fsync(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FDS) {
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    int pid = process_find_current();
    if (pid < 0) {
        return pid;
    }

    struct process *p = &process_table[pid];
    unsigned long fdflags = spin_lock_irqsave(&p->fd_lock);
    struct vfs_file *f = p->fd_table[fd];
    if (!f || !f->node) {
        spin_unlock_irqrestore(&p->fd_lock, fdflags);
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }
    struct vfs_vnode *node = f->node;
    atomic_inc(&node->refcount);
    spin_unlock_irqrestore(&p->fd_lock, fdflags);

    int result = pagecache_writeback(node);

    vfs_vnode_put(node);
    return result >= 0 ? 0 : result;
}
