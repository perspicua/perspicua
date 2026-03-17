#include "fs/pipe.h"
#include "core/lock.h"
#include "sched/process.h"
#include "mm/slab.h"
#include "core/timer.h"
#include "uapi/errors.h"
#include "stdio.h"
#include "string.h"
#include "mm/heap.h"

/* Helper to block the current task on a pipe queue */
static void pipe_wait(struct task** queue, spinlock_t* lock)
{
    struct task* self = sched_get_current();
    unsigned long flags = irq_save();

    /* Add self to the wait queue (simple linked list) */
    self->next = *queue;
    *queue = self;

    /* Set state to BLOCKED while holding the pipe lock */
    self->state = SCHED_TASK_BLOCKED;

    /* Release the pipe lock and the CPU */
    spin_unlock(lock);
    schedule();

    /* After waking up, we need to re-acquire the pipe lock to continue safely.
     * Note: schedule() restored IRQs, so we use spin_lock instead of irqsave here.
     */
    irq_restore(flags);
    spin_lock(lock);
}

/* Helper to wake up all tasks on a pipe queue */
static void pipe_wake(struct task** queue)
{
    struct task* t = *queue;
    while (t)
    {
        struct task* next = t->next;
        sched_unblock(t);
        t = next;
    }
    *queue = (void*)0;
}

static int pipe_read(struct vfs_file* file, void* buffer, size_t count)
{
    struct vfs_vnode* node = file->node;
    struct pipe* pipe = (struct pipe*)node->internal_info;
    char* buf = (char*)buffer;
    size_t read = 0;

    if (!pipe)
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;

    spin_lock(&pipe->lock);

    while (read < count)
    {
        if (pipe->count > 0)
        {
            /* Copy data from circular buffer */
            buf[read++] = pipe->buffer[pipe->tail];
            pipe->tail = (pipe->tail + 1) % PIPE_BUF_SIZE;
            pipe->count--;
        }
        else
        {
            /* Buffer is empty */
            if (read > 0)
            {
                /* Already read some data, return what we have */
                break;
            }

            if (pipe->writers == 0)
            {
                /* EOF: No more writers and buffer is empty */
                break;
            }

            /* Block until data is available */
            pipe_wait(&pipe->read_wait_queue, &pipe->lock);
        }
    }

    /* Wake up writers as space is now available */
    if (pipe->write_wait_queue)
    {
        pipe_wake(&pipe->write_wait_queue);
    }

    spin_unlock(&pipe->lock);
    return (int)read;
}

static int pipe_write(struct vfs_file* file, const void* buffer, size_t count)
{
    struct vfs_vnode* node = file->node;
    struct pipe* pipe = (struct pipe*)node->internal_info;
    const char* buf = (const char*)buffer;
    size_t written = 0;

    if (!pipe)
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;

    spin_lock(&pipe->lock);

    while (written < count)
    {
        if (pipe->readers == 0)
        {
            /* Broken pipe */
            spin_unlock(&pipe->lock);
            return -PERS_ERR_BROKEN_PIPE;
        }

        if (pipe->count < PIPE_BUF_SIZE)
        {
            /* Copy data to circular buffer */
            pipe->buffer[pipe->head] = buf[written++];
            pipe->head = (pipe->head + 1) % PIPE_BUF_SIZE;
            pipe->count++;
        }
        else
        {
            /* Buffer is full, block until space is available */
            pipe_wait(&pipe->write_wait_queue, &pipe->lock);
        }
    }

    /* Wake up readers as data is now available */
    if (pipe->read_wait_queue)
    {
        pipe_wake(&pipe->read_wait_queue);
    }

    spin_unlock(&pipe->lock);
    return (int)written;
}

static int pipe_close(struct vfs_file* file)
{
    struct vfs_vnode* node = file->node;
    struct pipe* pipe = (struct pipe*)node->internal_info;
    if (!pipe)
        return PERS_SUCCESS;

    int is_write = (file->flags & VFS_O_ACCMODE) != VFS_O_RDONLY;

    spin_lock(&pipe->lock);
    if (is_write)
        pipe->writers--;
    else
        pipe->readers--;

    /* Wake up everyone because someone closed an end */
    pipe_wake(&pipe->read_wait_queue);
    pipe_wake(&pipe->write_wait_queue);

    int destroy = (pipe->readers == 0 && pipe->writers == 0);
    spin_unlock(&pipe->lock);

    if (destroy)
    {
        heap_free(pipe);
        node->internal_info = (void*)0;
    }

    return PERS_SUCCESS;
}

/* Pipe operations table */
struct vfs_vnode_ops pipe_ops = {.read = pipe_read, .write = pipe_write, .lookup = (void*)0, .close = pipe_close};

/*
 * kernel_pipe - Internal implementation of the pipe() system call.
 */
int kernel_pipe(int pipefd[2])
{
    int pid = process_find_current();
    if (pid < 0)
        return pid;

    struct process* p = &process_table[pid];

    /* 1. Allocate the shared pipe structure */
    struct pipe* pipe = (struct pipe*)heap_malloc(sizeof(struct pipe));
    if (!pipe)
        return -PERS_ERR_OUT_OF_MEMORY;

    memset(pipe, 0, sizeof(struct pipe));
    pipe->readers = 1;
    pipe->writers = 1;
    pipe->lock.locked = 0;

    /* 2. Allocate the vnode that represents this pipe */
    struct vfs_vnode* node = (struct vfs_vnode*)slab_alloc(sizeof(struct vfs_vnode));
    if (!node)
    {
        heap_free(pipe);
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    memset(node, 0, sizeof(struct vfs_vnode));
    node->type = VFS_VNODE_TYPE_REGULAR; /* Pipes are "files" */
    node->ops = &pipe_ops;
    node->internal_info = pipe;
    node->refcount.counter = 2; /* One for each file descriptor */

    /* 3. Create the two file objects */
    struct vfs_file* f_read = (struct vfs_file*)slab_alloc(sizeof(struct vfs_file));
    struct vfs_file* f_write = (struct vfs_file*)slab_alloc(sizeof(struct vfs_file));

    if (!f_read || !f_write)
    {
        if (f_read)
            slab_free(f_read);
        if (f_write)
            slab_free(f_write);
        slab_free(node);
        heap_free(pipe);
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    f_read->node = node;
    f_read->flags = VFS_O_RDONLY;
    f_read->offset = 0;
    f_read->refcount.counter = 1;

    f_write->node = node;
    f_write->flags = VFS_O_WRONLY;
    f_write->offset = 0;
    f_write->refcount.counter = 1;

    /* 4. Assign file descriptors in the process table */
    int fd_r = -1, fd_w = -1;
    spin_lock(&p->fd_lock);

    for (int i = 0; i < VFS_MAX_FDS; i++)
    {
        if (!p->fd_table[i])
        {
            if (fd_r == -1)
                fd_r = i;
            else if (fd_w == -1)
            {
                fd_w = i;
                break;
            }
        }
    }

    if (fd_r != -1 && fd_w != -1)
    {
        p->fd_table[fd_r] = f_read;
        p->fd_table[fd_w] = f_write;
    }
    spin_unlock(&p->fd_lock);

    if (fd_r == -1 || fd_w == -1)
    {
        /* Cleanup on failure to find slots */
        slab_free(f_read);
        slab_free(f_write);
        slab_free(node);
        heap_free(pipe);
        return -PERS_ERR_OUT_OF_RESOURCES;
    }

    pipefd[0] = fd_r;
    pipefd[1] = fd_w;

    return PERS_SUCCESS;
}
