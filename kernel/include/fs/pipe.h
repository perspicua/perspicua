/*
 * pipe.h - Public API for anonymous pipes.
 */

#ifndef PERSPICUA_FS_PIPE_H
#define PERSPICUA_FS_PIPE_H

/*
 * pipe_create - Internal implementation of the pipe() system call.
 *
 * Allocates a shared buffer and two file descriptors (read/write) for
 * the current process. Returns 0 or a negative error.
 */
int pipe_create(int pipefd[2]);

#endif // PERSPICUA_FS_PIPE_H
