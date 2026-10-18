/*
 * wait.h - waitpid options and the status word it fills in.
 */

#ifndef PERSPICUA_UAPI_WAIT_H
#define PERSPICUA_UAPI_WAIT_H

#define WNOHANG   1
#define WUNTRACED 2

// The low 8 bits are the exit code, or 128 + signal; a bit above them marks a stop.
#define __W_STOPPED 0x10000

#define WIFSTOPPED(s)  (((s) & __W_STOPPED) != 0)
#define WSTOPSIG(s)    ((s) & 0xFF)
#define WIFEXITED(s)   (((s) & __W_STOPPED) == 0)
#define WEXITSTATUS(s) ((s) & 0xFF)

#endif // PERSPICUA_UAPI_WAIT_H
