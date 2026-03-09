#ifndef _VFS_H_
#define _VFS_H_

#include "lib/types.h"
struct vnode;
struct file;

#define MAX_PATH_LEN 4096
#define MAX_FDS 32
#define MAX_MOUNTS 8

typedef enum
{
    VNODE_TYPE_REGULAR,
    VNODE_TYPE_DIR,
    VNODE_TYPE_DEVICE
} vnode_type_t;

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002
#define O_ACCMODE 0x0003

#define O_CREAT 0x0100
#define O_TRUNC 0x0200
#define O_APPEND 0x0400

struct mount_entry
{
    char path[MAX_PATH_LEN];
    struct vnode* root;
};

struct vnode
{
    vnode_type_t type;
    size_t filesize;
    struct vnode_ops* ops;
    void* internal_info;
};

struct vnode_ops
{
    int (*read)(struct file* file, void* buffer, size_t size);
    int (*write)(struct file* file, const void* buffer, size_t size);
    struct vnode* (*lookup)(struct vnode* dir, const char* filename);
};

struct file
{
    struct vnode* node;
    uint32_t offset;
    int flags;
};

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// intialize vfs system
void vfs_init(void);

// walks a path and resolves the target vnode, or NULL if not found
struct vnode* vfs_resolve_path(const char* path);

// open a file, returns file descriptor if succesfull, -1 otherwise
int vfs_open(const char* path, int flags);

// open a file for a specific process by pid
int vfs_open_pid(const char* path, int flags, uint32_t pid);

// close a file, return 0 on succes
int vfs_close(int fd);

// seek in a file. returns the new offset
int vfs_lseek(int fd, int offset, int whence);

// reads from a file. returns the bytes read from the file
int vfs_read(int fd, void* buffer, size_t count);

// wrties to a file. returns the bytes written to the file
int vfs_write(int fd, const void* buffer, size_t count);

// mount a filesystem's root vnode at given path. returns 0 on success, -1 on error
int vfs_mount(const char* path, struct vnode* root);

// unmont the filesystem at a given path. return 0 on success, -1 on error
int vfs_unmount(const char* path);

#endif // _VFS_H_
