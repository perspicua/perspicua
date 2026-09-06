/*
 * vfs.h - Public API for the Virtual Filesystem (VFS) layer.
 *
 * This header defines the core filesystem abstractions, including vnodes,
 * file objects, and the functional interface for filesystem drivers.
 */

#ifndef PERSPICUA_FS_VFS_H
#define PERSPICUA_FS_VFS_H

#include "types.h"

#include "uapi/stat.h"

#include "core/lock.h"

#define VFS_MAX_PATH_LEN 4096

/*
 * Mount points are short ("/", "/dev"), and the mount table is static, so
 * storing a full-length path per entry costs 4 KB each for nothing.
 */
#define VFS_MAX_MOUNT_PATH 64
#ifdef CONFIG_MAX_FDS
    #define VFS_MAX_FDS CONFIG_MAX_FDS
#else
    #define VFS_MAX_FDS 64
#endif
#define VFS_MAX_MOUNTS 8

/* Standard file open flags */
#define VFS_O_RDONLY  0x0000
#define VFS_O_WRONLY  0x0001
#define VFS_O_RDWR    0x0002
#define VFS_O_ACCMODE 0x0003

#define VFS_O_CREAT    0x0100
#define VFS_O_TRUNC    0x0200
#define VFS_O_APPEND   0x0400
#define VFS_O_CLOEXEC  0x0800
#define VFS_O_NONBLOCK 0x1000

/* fcntl commands */
#define VFS_F_GETFD 1
#define VFS_F_SETFD 2
#define VFS_F_GETFL 3
#define VFS_F_SETFL 4

/* fcntl file descriptor flags */
#define VFS_FD_CLOEXEC 1

/* Seek mode constants */
#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

typedef int64_t vfs_off_t;

/*
 * enum vfs_vnode_type - Categories of filesystem objects.
 */
enum vfs_vnode_type {
    VFS_VNODE_TYPE_REGULAR,
    VFS_VNODE_TYPE_DIR,
    VFS_VNODE_TYPE_DEVICE
};

struct vfs_vnode;
struct vfs_file;

/*
 * struct vfs_dirent - Directory entry format returned to userspace.
 */
struct vfs_dirent {
    uint32_t ino;
    char name[256];
};

/*
 * struct vfs_vnode_ops - Functional interface for filesystem-specific operations.
 */
struct vfs_vnode_ops {
    int (*read)(struct vfs_file *file, void *buffer, size_t size);
    int (*write)(struct vfs_file *file, const void *buffer, size_t size);
    int (*truncate)(struct vfs_vnode *node, vfs_off_t length);
    struct vfs_vnode *(*lookup)(struct vfs_vnode *dir, const char *filename);
    int (*readdir)(struct vfs_file *file, void *buffer, size_t count);
    int (*close)(struct vfs_file *file);
    int (*stat)(struct vfs_vnode *node, struct stat *buf);
    int (*mmap)(struct vfs_file *file, uintptr_t vaddr, size_t length, int prot, int flags);
    int (*write_page)(struct vfs_vnode *node, size_t page_index, void *data, size_t valid_bytes);
    int (*mkdir)(struct vfs_vnode *parent, const char *name);
    int (*create)(struct vfs_vnode *parent, const char *name);
    int (*rmdir)(struct vfs_vnode *parent, const char *name);
    int (*unlink)(struct vfs_vnode *parent, const char *name);
    int (*rename)(struct vfs_vnode *old_parent, const char *old_name, struct vfs_vnode *new_parent,
                  const char *new_name);
};

/*
 * struct vfs_vnode - The primary VFS object representing a file or directory.
 */
struct vfs_vnode {
    enum vfs_vnode_type type;
    char name[256];
    vfs_off_t file_size;
    struct vfs_vnode *parent;
    struct vfs_vnode_ops *ops;
    void *internal_info;
    atomic_t refcount;
};

/*
 * struct vfs_file - An open instance of a vnode (file descriptor state).
 */
struct vfs_file {
    struct vfs_vnode *node;
    vfs_off_t offset;
    int flags;
    atomic_t refcount;
};

/*
 * vfs_init - Initializes the mount table and VFS synchronization.
 */
void vfs_init(void);

/*
 * vfs_resolve_path - Traverses the directory tree to find a specific vnode.
 */
struct vfs_vnode *vfs_resolve_path(const char *path, struct vfs_vnode *cwd, int *error);

/*
 * vfs_vnode_put - Safely releases a vnode reference.
 */
void vfs_vnode_put(struct vfs_vnode *node);

/*
 * vfs_file_alloc - Allocates an open-file object holding one reference.
 */
struct vfs_file *vfs_file_alloc(void);

/*
 * vfs_file_put - Drops a reference, releasing the file with the last one.
 *
 * The only correct way to release a struct vfs_file: dropping the count
 * without this runs neither the driver's close nor the vnode put.
 */
void vfs_file_put(struct vfs_file *f);

#ifdef CONFIG_TESTS
/* Open-file objects allocated but not yet released. */
unsigned long vfs_test_live_files(void);

/* The open-file object behind a descriptor, or NULL. */
struct vfs_file *vfs_test_file_at(int fd);
#endif

/*
 * vfs_open - Standard process-relative file open.
 */
int vfs_open(const char *path, int flags);

/*
 * vfs_open_pid - Kernel-internal open for a specific process context.
 */
int vfs_open_pid(const char *path, int flags, uint32_t pid);

/*
 * vfs_close - Closes a file descriptor and performs cleanup.
 */
int vfs_close(int fd);

/*
 * vfs_lseek - Repositions the read/write cursor.
 */
vfs_off_t vfs_lseek(int fd, vfs_off_t offset, int whence);

/*
 * vfs_read - Synchronous data retrieval from a descriptor.
 */
int vfs_read(int fd, void *buffer, size_t count);

/*
 * vfs_pread - Synchronous data retrieval from a descriptor at a specific offset.
 */
int vfs_pread(int fd, void *buffer, size_t count, vfs_off_t offset);

/*
 * vfs_write - Synchronous data submission to a descriptor.
 */
int vfs_write(int fd, const void *buffer, size_t count);

/*
 * vfs_pwrite - Synchronous data submission to a descriptor at a specific offset.
 */
int vfs_pwrite(int fd, const void *buffer, size_t count, vfs_off_t offset);

/*
 * vfs_readdir - Retrieves directory entries from a descriptor.
 */
int vfs_readdir(int fd, void *buffer, size_t count);

/*
 * vfs_stat - Retrieves metadata for a file by path.
 */
int vfs_stat(const char *path, struct stat *buf);

/*
 * vfs_fstat - Retrieves metadata for a file by file descriptor.
 */
int vfs_fstat(int fd, struct stat *buf);
int vfs_truncate(const char *path, vfs_off_t length);
int vfs_ftruncate(int fd, vfs_off_t length);

/*
 * vfs_mkdir - Creates a new directory from a given path.
 */
int vfs_mkdir(const char *path);

/*
 *  vfs_rmdir - Removes an existing directory from a given path.
 */
int vfs_rmdir(const char *path);

int vfs_unlink(const char *path);

int vfs_rename(const char *oldpath, const char *newpath);

/*
 * vfs_fsync - Flushes all dirty cached pages for a single file descriptor to disk.
 */
int vfs_fsync(int fd);

/*
 * vfs_dup2 - Duplicates a descriptor to a specific target slot.
 */
int vfs_dup2(int oldfd, int newfd);

/*
 * vfs_mount - Attaches a filesystem root to the global namespace.
 */
int vfs_mount(const char *path, struct vfs_vnode *root);

/*
 * vfs_unmount - Detaches a mount point by its path.
 */
int vfs_unmount(const char *path);

/*
 * vfs_get_mount - Copies the path of mount #index into out.
 */
int vfs_get_mount(size_t index, char *out, size_t out_size);

/*
 * vfs_chdir - Updates the current process working directory.
 */
int vfs_chdir(const char *path);

/*
 * vfs_getcwd - Returns the absolute path of the current working directory.
 */
int vfs_getcwd(char *buf, size_t size);

#endif /* PERSPICUA_FS_VFS_H */
