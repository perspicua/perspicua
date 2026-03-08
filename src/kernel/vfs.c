#include "vfs.h"
#include "../lib/string.h"
#include "heap.h"

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
