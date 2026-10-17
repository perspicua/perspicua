/*
 * pipe.h - Public API for anonymous pipes.
 */

#ifndef PERSPICUA_FS_PIPE_H
#define PERSPICUA_FS_PIPE_H

struct vfs_vnode;

// Capacity, and the largest write that is never interleaved with another.
#define PIPE_BUF_SIZE 4096

/*
 * pipe_create - Internal implementation of the pipe() system call.
 *
 * Allocates a shared buffer and two file descriptors (read/write) for
 * the current process. Returns 0 or a negative error.
 */
int pipe_create(int pipefd[2]);

// 1 if node is a pipe end, which has no position to seek.
int pipe_is_pipe(const struct vfs_vnode *node);

#endif // PERSPICUA_FS_PIPE_H
