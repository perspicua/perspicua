/*
 * pipe.h - Public API for anonymous pipes.
 */

#ifndef PERSPICUA_FS_PIPE_H
#define PERSPICUA_FS_PIPE_H

#include <limits.h>

struct vfs_vnode;

// Capacity, and the largest write that is never interleaved with another: POSIX's PIPE_BUF.
#define PIPE_BUF_SIZE PIPE_BUF

// pipe(): a shared buffer and a read and a write descriptor for the current process.
int pipe_create(int pipefd[2]);

// 1 if node is a pipe end, which has no position to seek.
int pipe_is_pipe(const struct vfs_vnode *node);

#endif // PERSPICUA_FS_PIPE_H
