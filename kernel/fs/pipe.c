/*
 * pipe.c - Implementation of anonymous unidirectional pipes.
 */

#include "fs/pipe.h"

#include <stddef.h>

#include "stdio.h"
#include "string.h"
#include "panic.h"

#include "uapi/errno.h"

#include "core/lock.h"
#include "core/signals.h"
#include "mm/slab.h"
#include "mm/heap.h"
#include "fs/vfs.h"
#include "sched/sched.h"
#include "sched/process.h"
#include "sched/wait.h"

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

    struct wait_queue read_wq;
    struct wait_queue write_wq;
};

static int pipe_read(struct vfs_file *file, void *buffer, size_t count, vfs_off_t *offset)
{
    (void)offset; // A pipe has no position.

    struct pipe *pipe = (struct pipe *)file->node->internal_info;
    char *buf = (char *)buffer;
    size_t read = 0;

    if (!pipe) {
        return -EBADF;
    }

    unsigned long fdflags = spin_lock_irqsave(&pipe->lock);

    while (read < count) {
        if (pipe->count > 0) {
            buf[read++] = pipe->buffer[pipe->tail];
            pipe->tail = (pipe->tail + 1) % PIPE_BUF_SIZE;
            pipe->count--;
        } else {
            if (read > 0 || pipe->writers == 0) {
                break;
            }
            if (file->flags & O_NONBLOCK) {
                if (read == 0) {
                    spin_unlock_irqrestore(&pipe->lock, fdflags);
                    return -EAGAIN;
                }
                break;
            }
            /* A partial read is a result the caller must see; only an empty
             * one can be restarted. */
            int r = wq_wait_event_interruptible_locked(
                &pipe->read_wq, pipe->count > 0 || pipe->writers == 0, &pipe->lock);
            if (r != 0) {
                spin_unlock_irqrestore(&pipe->lock, fdflags);
                return r;
            }
        }
    }

    wq_wake_all(&pipe->write_wq);

    spin_unlock_irqrestore(&pipe->lock, fdflags);
    return (int)read;
}

static int pipe_write(struct vfs_file *file, const void *buffer, size_t count, vfs_off_t *offset)
{
    (void)offset; // A pipe has no position.

    struct pipe *pipe = (struct pipe *)file->node->internal_info;
    const char *buf = (const char *)buffer;
    size_t written = 0;

    if (!pipe) {
        return -EBADF;
    }

    unsigned long fdflags = spin_lock_irqsave(&pipe->lock);

    while (written < count) {
        if (pipe->readers == 0) {
            spin_unlock_irqrestore(&pipe->lock, fdflags);
            // Without the signal a writer that ignores the error spins forever.
            signal_send((uint32_t)process_current_pid(), SIGPIPE);
            return written > 0 ? (int)written : -EPIPE;
        }

        if (pipe->count < PIPE_BUF_SIZE) {
            pipe->buffer[pipe->head] = buf[written++];
            pipe->head = (pipe->head + 1) % PIPE_BUF_SIZE;
            pipe->count++;
        } else {
            if (file->flags & O_NONBLOCK) {
                if (written == 0) {
                    spin_unlock_irqrestore(&pipe->lock, fdflags);
                    return -EAGAIN;
                }
                break;
            }
            // Wake waiting readers before blocking so writes larger than the buffer don't deadlock.
            wq_wake_all(&pipe->read_wq);

            int r = wq_wait_event_interruptible_locked(
                &pipe->write_wq, pipe->count < PIPE_BUF_SIZE || pipe->readers == 0, &pipe->lock);
            if (r != 0) {
                spin_unlock_irqrestore(&pipe->lock, fdflags);
                return written > 0 ? (int)written : r;
            }
        }
    }

    wq_wake_all(&pipe->read_wq);

    spin_unlock_irqrestore(&pipe->lock, fdflags);
    return (int)written;
}

static int pipe_close(struct vfs_file *file)
{
    struct pipe *pipe = (struct pipe *)file->node->internal_info;
    if (!pipe) {
        return 0;
    }

    int is_write = (file->flags & O_ACCMODE) != O_RDONLY;

    unsigned long fdflags = spin_lock_irqsave(&pipe->lock);
    if (is_write) {
        pipe->writers--;
    } else {
        pipe->readers--;
    }

    int destroy = (pipe->readers == 0 && pipe->writers == 0);

    /*
     * A waiter always holds its own endpoint open — readers block only while
     * writers != 0 and vice versa, and the vfs_file reference held across the
     * call keeps its own side from closing — so the last close cannot race one.
     * Checked before the wakes below empty the queues, because if this ever
     * stops holding, those wakes hand a freed pipe to a running task.
     */
    if (destroy && (pipe->read_wq.head || pipe->write_wq.head)) {
        PANIC("pipe: last close with waiters still queued");
    }

    wq_wake_all(&pipe->read_wq);
    wq_wake_all(&pipe->write_wq);

    if (destroy) {
        file->node->internal_info = NULL;
    }
    spin_unlock_irqrestore(&pipe->lock, fdflags);

    if (destroy) {
        heap_free(pipe);
    }

    return 0;
}

static struct vfs_vnode_ops pipe_ops = {
    .read = pipe_read, .write = pipe_write, .close = pipe_close};

int pipe_create(int pipefd[2])
{
    struct process *p = process_current();
    if (!p) {
        return -ESRCH;
    }

    struct pipe *pipe = (struct pipe *)heap_malloc(sizeof(struct pipe));
    if (!pipe) {
        return -ENOMEM;
    }

    memset(pipe, 0, sizeof(struct pipe));
    pipe->readers = 1;
    pipe->writers = 1;
    wq_init(&pipe->read_wq);
    wq_init(&pipe->write_wq);

    struct vfs_vnode *node = (struct vfs_vnode *)slab_alloc(sizeof(struct vfs_vnode));
    if (!node) {
        heap_free(pipe);
        return -ENOMEM;
    }

    memset(node, 0, sizeof(struct vfs_vnode));
    node->type = VFS_VNODE_TYPE_REGULAR;
    node->ops = &pipe_ops;
    node->internal_info = pipe;
    atomic_set(&node->refcount, 2);

    struct vfs_file *f_read = vfs_file_alloc();
    struct vfs_file *f_write = vfs_file_alloc();

    if (!f_read || !f_write) {
        // No vnode attached yet, so these just free the objects.
        vfs_file_put(f_read);
        vfs_file_put(f_write);
        slab_free(node);
        heap_free(pipe);
        return -ENOMEM;
    }

    f_read->node = node;
    f_read->flags = O_RDONLY;

    f_write->node = node;
    f_write->flags = O_WRONLY;

    int fd_r = -1, fd_w = -1;
    unsigned long fdflags = spin_lock_irqsave(&p->fd_lock);

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
    spin_unlock_irqrestore(&p->fd_lock, fdflags);

    if (fd_r == -1 || fd_w == -1) {
        /* Both ends are attached now, so releasing them runs pipe_close and
         * drops the last vnode reference, taking the pipe and node with it. */
        vfs_file_put(f_read);
        vfs_file_put(f_write);
        return -ENFILE;
    }

    pipefd[0] = fd_r;
    pipefd[1] = fd_w;

    return 0;
}
