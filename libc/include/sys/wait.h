/*
 * sys/wait.h - Waiting on child processes.
 */

#ifndef PERSPICUA_LIBC_SYS_WAIT_H
#define PERSPICUA_LIBC_SYS_WAIT_H

#include "uapi/wait.h"

int waitpid(int pid, int *status, int options);

#endif // PERSPICUA_LIBC_SYS_WAIT_H
