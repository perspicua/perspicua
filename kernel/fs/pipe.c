/*
 * pipe.c - Implementation of anonymous pipes (IPC).
 *
 * This module manages the circular buffer and task synchronization required
 * for unidirectional inter-process communication.
 */

#include "fs/pipe.h"

#include "stdio.h"
#include "string.h"

#include "uapi/errors.h"

#include "core/lock.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "mm/heap.h"
#include "fs/vfs.h"
#include "sched/sched.h"
#include "sched/process.h"

#define PIPE_BUF_SIZE 4096

/*
 * struct pipe - Shared IPC buffer and synchronization state.
 */
struct pipe {
    char buffer[PIPE_BUF_SIZE];
    size_t head;
    size_t tail;
    size_t count;

    int readers;
    int writers;

    spinlock_t lock;

    struct task *read_wait_queue;
    struct task *write_wait_queue;
};

/*
 * pipe_queue_remove - Unlinks a task from a pipe wait queue if still present.
 *
 * A task woken by a signal (sched_unblock) rather than pipe_wake stays linked
 * via wait_next; it must be removed before it can re-enqueue or be freed.
 */
static void pipe_queue_remove(struct task **queue, struct task *t)
{
    while (*queue) {
        if (*queue == t) {
            *queue = t->wait_next;
            t->wait_next = NULL;
            return;
        }
        queue = &(*queue)->wait_next;
    }
}

/*
 * pipe_signal_pending - True if the caller has an unblocked pending signal.
 */
static int pipe_signal_pending(void)
{
    int pid = process_find_current();
    if (pid < 0) {
        return 0;
    }
    struct process *p = &process_table[pid];
    return (p->pending_signals & ~p->blocked_signals) != 0;
}

/*
 * pipe_wait - Blocks the current task on a pipe's specific wait queue.
 */
static void pipe_wait(struct task **queue, spinlock_t *lock)
{
    struct task *self = sched_get_current();
    unsigned long flags = irq_save();

    /* Transition to BLOCKED before releasing lock to avoid lost wake-ups */
    self->state = SCHED_TASK_BLOCKED;
    self->wait_next = *queue;
    *queue = self;

    spin_unlock(lock);
    schedule();

    spin_lock(lock);
    /* A signal wake (rather than pipe_wake) leaves us queued: unlink now. */
    pipe_queue_remove(queue, self);
    irq_restore(flags);
}

/*
 * pipe_wake - Ready all tasks waiting on a pipe event.
 */
static void pipe_wake(struct task **queue)
{
    struct task *t = *queue;
    *queue = NULL; /* Prevent races with new waiters during unblocking */

    while (t) {
        struct task *next = t->wait_next;
        t->wait_next = NULL;
        sched_unblock(t);
        t = next;
    }
}

static int pipe_read(struct vfs_file *file, void *buffer, size_t count)
{
    struct pipe *pipe = (struct pipe *)file->node->internal_info;
    char *buf = (char *)buffer;
    size_t read = 0;

    if (!pipe) {
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    spin_lock(&pipe->lock);

    while (read < count) {
        if (pipe->count > 0) {
            buf[read++] = pipe->buffer[pipe->tail];
            pipe->tail = (pipe->tail + 1) % PIPE_BUF_SIZE;
            pipe->count--;
        } else {
            if (read > 0 || pipe->writers == 0) {
                break;
            }
            if (file->flags & VFS_O_NONBLOCK) {
                if (read == 0) {
                    spin_unlock(&pipe->lock);
                    return -PERS_ERR_TRY_AGAIN;
                }
                break;
            }
            if (pipe_signal_pending()) {
                spin_unlock(&pipe->lock);
                return read > 0 ? (int)read : -PERS_ERR_INTERRUPTED;
            }
            pipe_wait(&pipe->read_wait_queue, &pipe->lock);
        }
    }

    if (pipe->write_wait_queue) {
        pipe_wake(&pipe->write_wait_queue);
    }

    spin_unlock(&pipe->lock);
    return (int)read;
}

static int pipe_write(struct vfs_file *file, const void *buffer, size_t count)
{
    struct pipe *pipe = (struct pipe *)file->node->internal_info;
    const char *buf = (const char *)buffer;
    size_t written = 0;

    if (!pipe) {
        return -PERS_ERR_BAD_FILE_DESCRIPTOR;
    }

    spin_lock(&pipe->lock);

    while (written < count) {
        if (pipe->readers == 0) {
            spin_unlock(&pipe->lock);
            return -PERS_ERR_BROKEN_PIPE;
        }

        if (pipe->count < PIPE_BUF_SIZE) {
            pipe->buffer[pipe->head] = buf[written++];
            pipe->head = (pipe->head + 1) % PIPE_BUF_SIZE;
            pipe->count++;
        } else {
            if (file->flags & VFS_O_NONBLOCK) {
                if (written == 0) {
                    spin_unlock(&pipe->lock);
                    return -PERS_ERR_TRY_AGAIN;
                }
                break;
            }
            if (pipe_signal_pending()) {
                spin_unlock(&pipe->lock);
                return written > 0 ? (int)written : -PERS_ERR_INTERRUPTED;
            }
            pipe_wait(&pipe->write_wait_queue, &pipe->lock);
        }
    }

    if (pipe->read_wait_queue) {
        pipe_wake(&pipe->read_wait_queue);
    }

    spin_unlock(&pipe->lock);
    return (int)written;
}

static int pipe_close(struct vfs_file *file)
{
    struct pipe *pipe = (struct pipe *)file->node->internal_info;
    if (!pipe) {
        return PERS_SUCCESS;
    }

    int is_write = (file->flags & VFS_O_ACCMODE) != VFS_O_RDONLY;

    spin_lock(&pipe->lock);
    if (is_write) {
        pipe->writers--;
    } else {
        pipe->readers--;
    }

    pipe_wake(&pipe->read_wait_queue);
    pipe_wake(&pipe->write_wait_queue);

    int destroy = (pipe->readers == 0 && pipe->writers == 0);
    spin_unlock(&pipe->lock);

    if (destroy) {
        heap_free(pipe);
        file->node->internal_info = NULL;
    }

    return PERS_SUCCESS;
}

static struct vfs_vnode_ops pipe_ops = {
    .read = pipe_read, .write = pipe_write, .close = pipe_close};

/*
 * pipe_create - Allocates a pipe and installs descriptors in the process table.
 */
int pipe_create(int pipefd[2])
{
    int pid = process_find_current();
    if (pid < 0) {
        return pid;
    }

    struct process *p = &process_table[pid];

    struct pipe *pipe = (struct pipe *)heap_malloc(sizeof(struct pipe));
    if (!pipe) {
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    memset(pipe, 0, sizeof(struct pipe));
    pipe->readers = 1;
    pipe->writers = 1;

    struct vfs_vnode *node = (struct vfs_vnode *)slab_alloc(sizeof(struct vfs_vnode));
    if (!node) {
        heap_free(pipe);
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    memset(node, 0, sizeof(struct vfs_vnode));
    node->type = VFS_VNODE_TYPE_REGULAR;
    node->ops = &pipe_ops;
    node->internal_info = pipe;
    node->refcount.counter = 2;

    struct vfs_file *f_read = (struct vfs_file *)slab_alloc(sizeof(struct vfs_file));
    struct vfs_file *f_write = (struct vfs_file *)slab_alloc(sizeof(struct vfs_file));

    if (!f_read || !f_write) {
        if (f_read) {
            slab_free(f_read);
        }
        if (f_write) {
            slab_free(f_write);
        }
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

    int fd_r = -1, fd_w = -1;
    spin_lock(&p->fd_lock);

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!p->fd_table[i]) {
            if (fd_r == -1) {
                fd_r = i;
            } else if (fd_w == -1) {
                fd_w = i;
                break;
            }
        }
    }

    if (fd_r != -1 && fd_w != -1) {
        p->fd_table[fd_r] = f_read;
        p->fd_flags[fd_r] = 0;
        p->fd_table[fd_w] = f_write;
        p->fd_flags[fd_w] = 0;
    }
    spin_unlock(&p->fd_lock);

    if (fd_r == -1 || fd_w == -1) {
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
