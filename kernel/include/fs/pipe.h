/*
 * pipe.h - Public API for anonymous pipes.
 *
 * This header defines the kernel-internal interface for creating and
 * managing inter-process communication pipes.
 */

#ifndef PERSPICUA_FS_PIPE_H
#define PERSPICUA_FS_PIPE_H

#include "types.h"

/*
 * pipe_create - Internal implementation of the pipe() system call.
 *
 * Allocates a shared buffer and two file descriptors (read/write) for
 * the current process. Returns PERS_SUCCESS or a negative error.
 */
int pipe_create(int pipefd[2]);

#endif /* PERSPICUA_FS_PIPE_H */
