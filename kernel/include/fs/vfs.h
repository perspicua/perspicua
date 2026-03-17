/*
 * vfs.h - Public API for the Virtual Filesystem (VFS) layer.
 *
 * This file defines the core filesystem abstractions, including vnodes,
 * file descriptors, mount points, and the standard filesystem operations.
 */

#ifndef PERSPICUA_KERNEL_VFS_H
#define PERSPICUA_KERNEL_VFS_H

#include "types.h"

/* Forward declarations for core VFS structures */
struct vfs_vnode;
struct vfs_file;

/* VFS Limits */
#define VFS_MAX_PATH_LEN 4096
#define VFS_MAX_FDS      32
#define VFS_MAX_MOUNTS   8

/*
 * vfs_vnode_type - Enumeration of supported vnode types.
 */
enum vfs_vnode_type
{
    VFS_VNODE_TYPE_REGULAR,
    VFS_VNODE_TYPE_DIR,
    VFS_VNODE_TYPE_DEVICE
};

/* Standard file open flags */
#define VFS_O_RDONLY  0x0000
#define VFS_O_WRONLY  0x0001
#define VFS_O_RDWR    0x0002
#define VFS_O_ACCMODE 0x0003

#define VFS_O_CREAT   0x0100
#define VFS_O_TRUNC   0x0200
#define VFS_O_APPEND  0x0400
#define VFS_O_CLOEXEC 0x0800

/*
 * vfs_mount_entry - Represents a mounted filesystem instance.
 */
struct vfs_mount_entry
{
    char path[VFS_MAX_PATH_LEN];
    struct vfs_vnode* root;
};

/* Type definition for file offsets and sizes */
typedef int64_t vfs_off_t;

/*
 * vfs_dirent - Directory entry structure returned to userspace.
 */
struct vfs_dirent
{
    uint32_t ino;
    char name[256];
};

/*
 * vfs_vnode_ops - Functional interface for filesystem-specific operations.
 */
struct vfs_vnode_ops
{
    int (*read)(struct vfs_file* file, void* buffer, size_t size);
    int (*write)(struct vfs_file* file, const void* buffer, size_t size);
    struct vfs_vnode* (*lookup)(struct vfs_vnode* dir, const char* filename);
    int (*readdir)(struct vfs_file* file, void* buffer, size_t count);
    int (*close)(struct vfs_file* file);
};

/*
 * vfs_vnode - The primary VFS abstraction representing an inode/file.
 */
struct vfs_vnode
{
    enum vfs_vnode_type type;
    vfs_off_t file_size;
    struct vfs_vnode* parent;
    struct vfs_vnode_ops* ops;
    void* internal_info;
    atomic_t refcount;
};

/*
 * vfs_file - Represents an open file instance (file descriptor).
 */
struct vfs_file
{
    struct vfs_vnode* node;
    vfs_off_t offset;
    int flags;
    atomic_t refcount;
};

/* Seek mode constants */
#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

/*
 * vfs_init - Initializes the virtual filesystem structures and mount table.
 */
void vfs_init(void);

/*
 * vfs_resolve_path - Traverses the directory tree to find the vnode
 * corresponding to the given path.
 */
struct vfs_vnode* vfs_resolve_path(const char* path, struct vfs_vnode* cwd, int* error);

/*
 * vfs_vnode_put - Decrements the reference count of a vnode and frees it
 * if the count reaches zero.
 */
void vfs_vnode_put(struct vfs_vnode* node);

/*
 * vfs_open - Opens a file for the current process. Returns a file descriptor
 * on success or a negative error code.
 */
int vfs_open(const char* path, int flags);

/*
 * vfs_open_pid - Opens a file on behalf of a specific process.
 */
int vfs_open_pid(const char* path, int flags, uint32_t pid);

/*
 * vfs_close - Closes an open file descriptor.
 */
int vfs_close(int fd);

/*
 * vfs_lseek - Repositions the read/write offset of a file descriptor.
 */
vfs_off_t vfs_lseek(int fd, vfs_off_t offset, int whence);

/*
 * vfs_read - Reads data from a file descriptor into a buffer.
 */
int vfs_read(int fd, void* buffer, size_t count);

/*
 * vfs_readdir - Reads directory entries from a directory file descriptor.
 */
int vfs_readdir(int fd, void* buffer, size_t count);

/*
 * vfs_write - Writes data from a buffer to a file descriptor.
 */
int vfs_write(int fd, const void* buffer, size_t count);

/*
 * vfs_dup2 - Duplicates a file descriptor.
 */
int vfs_dup2(int oldfd, int newfd);

/*
 * vfs_mount - Attaches a filesystem root vnode to the global namespace
 * at the specified path.
 */
int vfs_mount(const char* path, struct vfs_vnode* root);

/*
 * vfs_unmount - Detaches a filesystem from the global namespace.
 */
int vfs_unmount(const char* path);

int vfs_chdir(const char* path);
int vfs_getcwd(char* buf, size_t size);
#endif /* PERSPICUA_KERNEL_VFS_H */
