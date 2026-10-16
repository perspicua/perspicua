#ifndef PERSPICUA_UAPI_WAIT_H
#define PERSPICUA_UAPI_WAIT_H

#define WNOHANG   1
#define WUNTRACED 2

/*
 * An exit status is 8 bits: the code a process exited with, or 128 + signal for
 * one a signal killed, as sh reports it. A bit above that range marks a stop.
 */
#define __W_STOPPED 0x10000

#define WIFSTOPPED(s)  (((s) & __W_STOPPED) != 0)
#define WSTOPSIG(s)    ((s) & 0xFF)
#define WIFEXITED(s)   (((s) & __W_STOPPED) == 0)
#define WEXITSTATUS(s) ((s) & 0xFF)

#endif // PERSPICUA_UAPI_WAIT_H
