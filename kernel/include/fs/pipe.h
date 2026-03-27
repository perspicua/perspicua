/*
 * pipe.h - Anonymous pipe definitions.
 */

#ifndef PERSPICUA_KERNEL_PIPE_H
#define PERSPICUA_KERNEL_PIPE_H

#include "types.h"
#include "fs/vfs.h"
#include "core/lock.h"
#include "sched/sched.h"

#define PIPE_BUF_SIZE 4096

/*
 * struct pipe - Represents an anonymous pipe.
 * This structure is typically pointed to by vnode->internal_info.
 */
struct pipe {
    char buffer[PIPE_BUF_SIZE];
    size_t head;  /* Next write position */
    size_t tail;  /* Next read position */
    size_t count; /* Number of bytes currently in buffer */

    int readers; /* Number of open read ends */
    int writers; /* Number of open write ends */

    spinlock_t lock;

    struct task *read_wait_queue;  /* Tasks waiting for data to read */
    struct task *write_wait_queue; /* Tasks waiting for space to write */
};

/* --- Public API --- */

/*
 * kernel_pipe - Internal implementation of the pipe() system call.
 * Creates a new anonymous pipe and returns two file descriptors.
 */
int kernel_pipe(int pipefd[2]);

#endif /* PERSPICUA_KERNEL_PIPE_H */
