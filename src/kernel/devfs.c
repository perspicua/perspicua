#include "kernel/devfs.h"
#include "kernel/heap.h"
#include "driver/uart.h"
#include "kernel/vfs.h"
#include "lib/string.h"

// for /dev/uart
struct vnode* devfs_root_vnode;
struct vnode* devfs_uart_vnode;
struct vnode* devfs_dev_vnode;

struct vnode_ops devfs_dev_ops;
struct vnode_ops devfs_root_ops;
struct vnode_ops devfs_uart_ops;

struct vnode* devfs_root_lookup(struct vnode* dir, const char* filename)
{
    if (dir != devfs_root_vnode)
        return NULL;

    if (strcmp(filename, "dev") == 0)
        return devfs_dev_vnode;

    return NULL;
}

struct vnode* devfs_dev_lookup(struct vnode* dir, const char* filename)
{
    if (dir != devfs_dev_vnode)
        return NULL;

    if (strcmp(filename, "uart") == 0)
        return devfs_uart_vnode;

    return NULL;
}

int devfs_uart_write(struct file* file, const void* buffer, size_t size)
{
    (void)file;
    char* char_buff = (char*)buffer;
    uart_write(char_buff, size);
    return (int)size;
}

int devfs_uart_read(struct file* file, void* buffer, size_t size)
{
    (void)file;
    char* char_buff = (char*)buffer;
    for (size_t i = 0; i < size; i++)
    {
        char c = uart_getc();
        if (c == '\r')
            c = '\n';

        char_buff[i] = c;
        uart_send(c);

        if (c == '\n')
            return (int)(i + 1);
    }
    return (int)size;
}

void devfs_init(void)
{
    devfs_root_vnode = (struct vnode*)kmalloc(sizeof(struct vnode));
    devfs_dev_vnode = (struct vnode*)kmalloc(sizeof(struct vnode));
    devfs_uart_vnode = (struct vnode*)kmalloc(sizeof(struct vnode));

    devfs_root_ops.lookup = devfs_root_lookup;
    devfs_root_ops.read = NULL;
    devfs_root_ops.write = NULL;

    devfs_dev_ops.lookup = devfs_dev_lookup;
    devfs_dev_ops.read = NULL;
    devfs_dev_ops.write = NULL;

    devfs_uart_ops.lookup = NULL;
    devfs_uart_ops.read = devfs_uart_read;
    devfs_uart_ops.write = devfs_uart_write;

    devfs_root_vnode->type = VNODE_TYPE_DIR;
    devfs_root_vnode->ops = &devfs_root_ops;

    devfs_dev_vnode->type = VNODE_TYPE_DIR;
    devfs_dev_vnode->ops = &devfs_dev_ops;

    devfs_uart_vnode->type = VNODE_TYPE_DEVICE;
    devfs_uart_vnode->ops = &devfs_uart_ops;

    vfs_set_root(devfs_root_vnode);
}
