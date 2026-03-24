#include "fs/procfs.h"
#include "core/lock.h"
#include "fs/vfs.h"
#include "mm/slab.h"
#include "string.h"
#include "stdio.h"
#include "uapi/errors.h"

#include "mm/pmm.h"
#include "sched/process.h"

static struct vfs_vnode* procfs_root_vnode = NULL;
static struct vfs_vnode* version_vnode = NULL;
static struct vfs_vnode* meminfo_vnode = NULL;

static int procfs_version_read(struct vfs_file* file, void* buffer, size_t size)
{
    const char* version_str = "perspicua kernel v0.1\n";
    size_t len = strlen(version_str);

    if (file->offset >= (vfs_off_t)len)
        return 0;

    size_t available = len - file->offset;
    size_t to_copy = (size < available) ? size : available;

    memcpy(buffer, version_str + file->offset, to_copy);
    file->offset += to_copy;

    return to_copy;
}

static int procfs_meminfo_read(struct vfs_file* file, void* buffer, size_t size)
{
    char buf[256];
    unsigned long total_pages = pmm_get_total_pages();
    unsigned long free_pages = pmm_get_free_pages();
    unsigned long slab_used = slab_get_used();
    unsigned long slab_total = slab_get_total();

    snprintf(buf,
             sizeof(buf),
             "MemTotal: %lu kB\n"
             "MemFree:  %lu kB\n"
             "SlabUsed: %lu kB\n"
             "SlabTotal:%lu kB\n",
             total_pages * 4,
             free_pages * 4,
             slab_used / 1024,
             slab_total / 1024);

    size_t len = strlen(buf);
    if (file->offset >= (vfs_off_t)len)
        return 0;

    size_t available = len - file->offset;
    size_t to_copy = (size < available) ? size : available;

    memcpy(buffer, buf + file->offset, to_copy);
    file->offset += to_copy;

    return to_copy;
}

static struct vfs_vnode_ops procfs_version_ops = {
    .read = procfs_version_read,
};

static struct vfs_vnode_ops procfs_meminfo_ops = {
    .read = procfs_meminfo_read,
};

/* Forward declarations for PID-specific ops */
static struct vfs_vnode* procfs_pid_lookup(struct vfs_vnode* dir, const char* filename);
static int procfs_pid_readdir(struct vfs_file* file, void* buffer, size_t count);

static struct vfs_vnode_ops procfs_pid_dir_ops = {
    .lookup = procfs_pid_lookup,
    .readdir = procfs_pid_readdir,
};

/* Helper to reconstruct a vnode's full path */
static size_t procfs_get_vnode_path(struct vfs_vnode* node, char* buf, size_t size)
{
    if (!node || !buf || size == 0)
        return 0;

    char* ptr = buf + size - 1;
    *ptr = '\0';

    struct vfs_vnode* curr = node;

    // Root case
    if (!curr->parent && curr->name[0] == '\0')
    {
        if (size < 2)
            return 0;
        ptr--;
        *ptr = '/';
    }
    else
    {
        while (curr && curr->parent)
        {
            size_t nlen = strlen(curr->name);
            if ((size_t)(ptr - buf) < nlen + 1)
                break;
            ptr -= nlen;
            memcpy(ptr, curr->name, nlen);
            ptr--;
            *ptr = '/';
            curr = curr->parent;
        }
        // If we didn't start at root and didn't reach root, it might be a relative path
        // but in this VFS, most things are anchored.
    }

    size_t len = strlen(ptr);
    if (len > 0)
    {
        memmove(buf, ptr, len + 1);
    }
    else
    {
        buf[0] = '\0';
    }
    return len;
}

static struct vfs_vnode* procfs_root_lookup(struct vfs_vnode* dir, const char* filename)
{
    if (strcmp(filename, "version") == 0)
    {
        atomic_inc(&version_vnode->refcount);
        return version_vnode;
    }
    if (strcmp(filename, "meminfo") == 0)
    {
        atomic_inc(&meminfo_vnode->refcount);
        return meminfo_vnode;
    }

    /* Check if filename is a PID */
    long pid = -1;
    const char* p = filename;
    if (*p >= '0' && *p <= '9')
    {
        pid = 0;
        while (*p >= '0' && *p <= '9')
        {
            pid = pid * 10 + (*p - '0');
            p++;
        }
        if (*p != '\0')
            pid = -1;
    }

    if (pid >= 0 && pid < PROCESS_TABLE_SIZE)
    {
        unsigned long flags = spin_lock_irqsave(&process_table_lock);
        if (process_table[pid].state != PROCESS_STATE_EMPTY)
        {
            spin_unlock_irqrestore(&process_table_lock, flags);
            struct vfs_vnode* node = (struct vfs_vnode*)slab_alloc(sizeof(struct vfs_vnode));
            memset(node, 0, sizeof(struct vfs_vnode));
            node->type = VFS_VNODE_TYPE_DIR;
            node->ops = &procfs_pid_dir_ops;
            node->refcount.counter = 1;
            node->internal_info = (void*)(uintptr_t)pid;
            node->parent = dir;
            snprintf(node->name, sizeof(node->name), "%ld", pid);
            return node;
        }
        spin_unlock_irqrestore(&process_table_lock, flags);
    }

    return NULL;
}

static int procfs_root_readdir(struct vfs_file* file, void* buffer, size_t count)
{
    struct vfs_dirent* vfs_buffer = (struct vfs_dirent*)buffer;
    size_t max_entries = count / sizeof(struct vfs_dirent);
    int entries_written = 0;

    while (entries_written < max_entries)
    {
        int current_idx = 0;
        int written = 0;

        if (file->offset == current_idx++)
        {
            vfs_buffer[entries_written].ino = 1;
            strcpy(vfs_buffer[entries_written].name, ".");
            file->offset++;
            written = 1;
        }
        else if (file->offset == current_idx++)
        {
            vfs_buffer[entries_written].ino = 1;
            strcpy(vfs_buffer[entries_written].name, "..");
            file->offset++;
            written = 1;
        }
        else if (file->offset == current_idx++)
        {
            vfs_buffer[entries_written].ino = 2;
            strcpy(vfs_buffer[entries_written].name, "version");
            file->offset++;
            written = 1;
        }
        else if (file->offset == current_idx++)
        {
            vfs_buffer[entries_written].ino = 3;
            strcpy(vfs_buffer[entries_written].name, "meminfo");
            file->offset++;
            written = 1;
        }
        else
        {
            int pid_offset = file->offset - current_idx;
            int found = 0;
            for (int i = pid_offset; i < PROCESS_TABLE_SIZE; i++)
            {
                unsigned long flags = spin_lock_irqsave(&process_table_lock);
                if (process_table[i].state != PROCESS_STATE_EMPTY)
                {
                    vfs_buffer[entries_written].ino = 100 + i;
                    snprintf(vfs_buffer[entries_written].name, sizeof(vfs_buffer[entries_written].name), "%d", i);
                    spin_unlock_irqrestore(&process_table_lock, flags);
                    file->offset++;
                    written = 1;
                    found = 1;
                    break;
                }
                spin_unlock_irqrestore(&process_table_lock, flags);
                file->offset++;
            }
            if (!found)
                break;  // EOF reached
        }

        if (written)
            entries_written++;
    }

    return entries_written;
}

static struct vfs_vnode_ops procfs_root_ops = {
    .readdir = procfs_root_readdir,
    .lookup = procfs_root_lookup,
};

static int procfs_pid_status_read(struct vfs_file* file, void* buffer, size_t size)
{
    uintptr_t pid = (uintptr_t)file->node->internal_info;
    char buf[512];

    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    if (process_table[pid].state == PROCESS_STATE_EMPTY)
    {
        spin_unlock_irqrestore(&process_table_lock, flags);
        return 0;
    }

    const char* state_str = "unknown";
    switch (process_table[pid].state)
    {
    case PROCESS_STATE_RUNNING:
        state_str = "running";
        break;
    case PROCESS_STATE_ZOMBIE:
        state_str = "zombie";
        break;
    case PROCESS_STATE_DEAD:
        state_str = "dead";
        break;
    default:
        break;
    }

    unsigned long vm_size = 0;
    for (size_t i = 0; i < process_table[pid].va.count; i++)
    {
        vm_size += process_table[pid].va.regions[i].pages * 4;
    }

    snprintf(buf,
             sizeof(buf),
             "Name:   %s\n"
             "Pid:    %lu\n"
             "PPid:   %u\n"
             "State:  %s\n"
             "VmSize: %lu kB\n",
             process_table[pid].name,
             pid,
             process_table[pid].parent_pid,
             state_str,
             vm_size);
    spin_unlock_irqrestore(&process_table_lock, flags);

    size_t len = strlen(buf);
    if (file->offset >= (vfs_off_t)len)
        return 0;

    size_t available = len - file->offset;
    size_t to_copy = (size < available) ? size : available;

    memcpy(buffer, buf + file->offset, to_copy);
    file->offset += to_copy;

    return to_copy;
}

static int procfs_pid_cmdline_read(struct vfs_file* file, void* buffer, size_t size)
{
    uintptr_t pid = (uintptr_t)file->node->internal_info;
    char buf[128];

    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    if (process_table[pid].state == PROCESS_STATE_EMPTY)
    {
        spin_unlock_irqrestore(&process_table_lock, flags);
        return 0;
    }
    snprintf(buf, sizeof(buf), "%s\n", process_table[pid].name);
    spin_unlock_irqrestore(&process_table_lock, flags);

    size_t len = strlen(buf);
    if (file->offset >= (vfs_off_t)len)
        return 0;

    size_t available = len - file->offset;
    size_t to_copy = (size < available) ? size : available;

    memcpy(buffer, buf + file->offset, to_copy);
    file->offset += to_copy;

    return to_copy;
}

static int procfs_pid_cwd_read(struct vfs_file* file, void* buffer, size_t size)
{
    uintptr_t pid = (uintptr_t)file->node->internal_info;
    struct process* p = &process_table[pid];

    struct vfs_vnode* cwd_node = NULL;
    spin_lock(&p->fd_lock);
    if (p->cwd)
    {
        cwd_node = p->cwd;
        atomic_inc(&cwd_node->refcount);
    }
    spin_unlock(&p->fd_lock);

    if (!cwd_node)
        return 0;

    char path_buf[VFS_MAX_PATH_LEN];
    size_t len = procfs_get_vnode_path(cwd_node, path_buf, sizeof(path_buf));
    vfs_vnode_put(cwd_node);

    if (len > 0 && len < sizeof(path_buf) - 1)
    {
        path_buf[len++] = '\n';
        path_buf[len] = '\0';
    }

    if (file->offset >= (vfs_off_t)len)
        return 0;

    size_t available = len - file->offset;
    size_t to_copy = (size < available) ? size : available;

    memcpy(buffer, path_buf + file->offset, to_copy);
    file->offset += to_copy;

    return to_copy;
}

static struct vfs_vnode_ops procfs_pid_status_ops = {
    .read = procfs_pid_status_read,
};

static struct vfs_vnode_ops procfs_pid_cmdline_ops = {
    .read = procfs_pid_cmdline_read,
};

static struct vfs_vnode_ops procfs_pid_cwd_ops = {
    .read = procfs_pid_cwd_read,
};

/* FD directory operations */
static struct vfs_vnode* procfs_pid_fd_lookup(struct vfs_vnode* dir, const char* filename);
static int procfs_pid_fd_readdir(struct vfs_file* file, void* buffer, size_t count);

static struct vfs_vnode_ops procfs_pid_fd_dir_ops = {
    .lookup = procfs_pid_fd_lookup,
    .readdir = procfs_pid_fd_readdir,
};

static int procfs_pid_fd_entry_read(struct vfs_file* file, void* buffer, size_t size)
{
    uintptr_t info = (uintptr_t)file->node->internal_info;
    uint32_t pid = (info >> 16) & 0xFFFF;
    uint32_t fd = info & 0xFFFF;

    if (pid >= PROCESS_TABLE_SIZE || fd >= VFS_MAX_FDS)
        return -PERS_ERR_INVALID_ARGUMENT;

    struct process* p = &process_table[pid];
    struct vfs_vnode* target_node = NULL;

    spin_lock(&p->fd_lock);
    if (p->fd_table[fd])
    {
        target_node = p->fd_table[fd]->node;
        atomic_inc(&target_node->refcount);
    }
    spin_unlock(&p->fd_lock);

    if (!target_node)
        return 0;

    char path_buf[VFS_MAX_PATH_LEN];
    size_t len = procfs_get_vnode_path(target_node, path_buf, sizeof(path_buf));
    vfs_vnode_put(target_node);

    if (len > 0 && len < sizeof(path_buf) - 1)
    {
        path_buf[len++] = '\n';
        path_buf[len] = '\0';
    }

    if (file->offset >= (vfs_off_t)len)
        return 0;

    size_t available = len - file->offset;
    size_t to_copy = (size < available) ? size : available;

    memcpy(buffer, path_buf + file->offset, to_copy);
    file->offset += to_copy;

    return to_copy;
}

static struct vfs_vnode_ops procfs_pid_fd_entry_ops = {
    .read = procfs_pid_fd_entry_read,
};

static struct vfs_vnode* procfs_pid_fd_lookup(struct vfs_vnode* dir, const char* filename)
{
    uint32_t pid = (uint32_t)(uintptr_t)dir->internal_info;
    uint32_t fd = 0;
    const char* p = filename;

    if (*p == '\0')
        return NULL;
    while (*p)
    {
        if (*p < '0' || *p > '9')
            return NULL;
        fd = fd * 10 + (*p - '0');
        p++;
    }

    if (fd >= VFS_MAX_FDS)
        return NULL;

    unsigned long flags = spin_lock_irqsave(&process_table_lock);
    if (process_table[pid].state == PROCESS_STATE_EMPTY)
    {
        spin_unlock_irqrestore(&process_table_lock, flags);
        return NULL;
    }

    spin_lock(&process_table[pid].fd_lock);
    if (!process_table[pid].fd_table[fd])
    {
        spin_unlock(&process_table[pid].fd_lock);
        spin_unlock_irqrestore(&process_table_lock, flags);
        return NULL;
    }
    spin_unlock(&process_table[pid].fd_lock);
    spin_unlock_irqrestore(&process_table_lock, flags);

    struct vfs_vnode* node = (struct vfs_vnode*)slab_alloc(sizeof(struct vfs_vnode));
    memset(node, 0, sizeof(struct vfs_vnode));
    node->type = VFS_VNODE_TYPE_REGULAR;
    node->ops = &procfs_pid_fd_entry_ops;
    node->refcount.counter = 1;
    node->internal_info = (void*)(uintptr_t)((pid << 16) | fd);
    node->parent = dir;
    strncpy(node->name, filename, sizeof(node->name) - 1);
    return node;
}

static int procfs_pid_fd_readdir(struct vfs_file* file, void* buffer, size_t count)
{
    struct vfs_dirent* vfs_buffer = (struct vfs_dirent*)buffer;
    uint32_t pid = (uint32_t)(uintptr_t)file->node->internal_info;
    size_t max_entries = count / sizeof(struct vfs_dirent);
    int entries_written = 0;

    while (entries_written < max_entries)
    {
        int written = 0;

        if (file->offset == 0)
        {
            vfs_buffer[entries_written].ino = 1;
            strcpy(vfs_buffer[entries_written].name, ".");
            file->offset++;
            written = 1;
        }
        else if (file->offset == 1)
        {
            vfs_buffer[entries_written].ino = 1;
            strcpy(vfs_buffer[entries_written].name, "..");
            file->offset++;
            written = 1;
        }
        else
        {
            int fd_idx = file->offset - 2;
            int found = 0;
            for (int i = fd_idx; i < VFS_MAX_FDS; i++)
            {
                struct process* p = &process_table[pid];
                spin_lock(&p->fd_lock);
                if (p->fd_table[i])
                {
                    vfs_buffer[entries_written].ino = 1000 + i;
                    snprintf(vfs_buffer[entries_written].name, sizeof(vfs_buffer[entries_written].name), "%d", i);
                    spin_unlock(&p->fd_lock);
                    file->offset++;
                    written = 1;
                    found = 1;
                    break;
                }
                spin_unlock(&p->fd_lock);
                file->offset++;
            }
            if (!found)
                break;  // EOF
        }

        if (written)
            entries_written++;
    }
    return entries_written;
}

static struct vfs_vnode* procfs_pid_lookup(struct vfs_vnode* dir, const char* filename)
{
    const char* names[] = {"status", "cmdline", "cwd", "fd"};
    struct vfs_vnode_ops* ops[] = {
        &procfs_pid_status_ops, &procfs_pid_cmdline_ops, &procfs_pid_cwd_ops, &procfs_pid_fd_dir_ops};
    enum vfs_vnode_type types[] = {
        VFS_VNODE_TYPE_REGULAR, VFS_VNODE_TYPE_REGULAR, VFS_VNODE_TYPE_REGULAR, VFS_VNODE_TYPE_DIR};

    for (size_t i = 0; i < 4; i++)
    {
        if (strcmp(filename, names[i]) == 0)
        {
            struct vfs_vnode* node = (struct vfs_vnode*)slab_alloc(sizeof(struct vfs_vnode));
            memset(node, 0, sizeof(struct vfs_vnode));
            node->type = types[i];
            node->ops = ops[i];
            node->refcount.counter = 1;
            node->internal_info = dir->internal_info;
            node->parent = dir;
            strcpy(node->name, names[i]);
            return node;
        }
    }
    return NULL;
}

static int procfs_pid_readdir(struct vfs_file* file, void* buffer, size_t count)
{
    struct vfs_dirent* vfs_buffer = (struct vfs_dirent*)buffer;
    const char* static_entries[] = {".", "..", "status", "cmdline", "cwd", "fd"};
    size_t max_entries = count / sizeof(struct vfs_dirent);
    int entries_written = 0;

    while (entries_written < max_entries && file->offset < 6)
    {
        vfs_buffer[entries_written].ino = file->offset + 1;
        strcpy(vfs_buffer[entries_written].name, static_entries[file->offset]);
        file->offset++;
        entries_written++;
    }
    return entries_written;
}
void procfs_init(void)
{
    procfs_root_vnode = (struct vfs_vnode*)slab_alloc(sizeof(struct vfs_vnode));
    version_vnode = (struct vfs_vnode*)slab_alloc(sizeof(struct vfs_vnode));
    meminfo_vnode = (struct vfs_vnode*)slab_alloc(sizeof(struct vfs_vnode));

    memset(procfs_root_vnode, 0, sizeof(struct vfs_vnode));
    procfs_root_vnode->type = VFS_VNODE_TYPE_DIR;
    procfs_root_vnode->ops = &procfs_root_ops;
    procfs_root_vnode->refcount.counter = 1;
    procfs_root_vnode->parent = NULL;
    strcpy(procfs_root_vnode->name, "proc");

    memset(version_vnode, 0, sizeof(struct vfs_vnode));
    version_vnode->type = VFS_VNODE_TYPE_REGULAR;
    version_vnode->ops = &procfs_version_ops;
    version_vnode->refcount.counter = 1;
    version_vnode->parent = procfs_root_vnode;
    strcpy(version_vnode->name, "version");

    memset(meminfo_vnode, 0, sizeof(struct vfs_vnode));
    meminfo_vnode->type = VFS_VNODE_TYPE_REGULAR;
    meminfo_vnode->ops = &procfs_meminfo_ops;
    meminfo_vnode->refcount.counter = 1;
    meminfo_vnode->parent = procfs_root_vnode;
    strcpy(meminfo_vnode->name, "meminfo");

    vfs_mount("/proc", procfs_root_vnode);

    pr_info("procfs: mounted at /proc\n");
}
