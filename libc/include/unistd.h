/*
 * unistd.h - POSIX process, file descriptor, and path operations.
 */

#ifndef PERSPICUA_LIBC_UNISTD_H
#define PERSPICUA_LIBC_UNISTD_H

#include <stddef.h>

#include "uapi/types.h"

// The descriptors every process starts with.
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// Process control
__attribute__((noreturn)) void _exit(int status);
int fork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
int getpid(void);
int getppid(void);
int setpgid(int pid, int pgid);
int getpgid(int pid);
int setsid(void);
int getsid(int pid);
int tcsetpgrp(int fd, int pgid);
int tcgetpgrp(int fd);

// File descriptors
int read(int fd, void *buf, size_t len);
int write(int fd, const char *buf, size_t len);
int pread(int fd, void *buf, size_t count, off_t offset);
int pwrite(int fd, const char *buf, size_t len, off_t offset);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int pipe(int pipefd[2]);
int dup2(int oldfd, int newfd);
int ftruncate(int fd, off_t length);
int fsync(int fd);

// Paths
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int unlink(const char *path);
int rmdir(const char *path);
int truncate(const char *path, off_t length);

// Directory reading, as the kernel hands entries out.
int getdents(int fd, void *buf, size_t count);

int sync(void);

unsigned int sleep(unsigned int seconds);
int usleep(useconds_t usec);

#endif // PERSPICUA_LIBC_UNISTD_H
