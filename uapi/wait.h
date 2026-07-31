#ifndef PERSPICUA_UAPI_WAIT_H
#define PERSPICUA_UAPI_WAIT_H

#define WNOHANG   1
#define WUNTRACED 2

/*
 * Exit statuses are small (process_exit stores at most 128 + signal), so a bit
 * well above that range marks a stop report without disturbing the raw status
 * convention every existing caller relies on.
 */
#define PERS_STATUS_STOPPED 0x10000

#define WIFSTOPPED(s) (((s) & PERS_STATUS_STOPPED) != 0)
#define WSTOPSIG(s)   ((s) & 0xFF)

#endif /* PERSPICUA_UAPI_WAIT_H */
