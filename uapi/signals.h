/*
 * signals.h - Public API for signals.
 *
 * This file defines the signals available and the handler type for
 * process signaling.
 */

#ifndef PERSPICUA_UAPI_SIGNALS_H
#define PERSPICUA_UAPI_SIGNALS_H

/* Signal numbers */
#define SIGNAL_HUP  1
#define SIGNAL_INT  2
#define SIGNAL_QUIT 3
#define SIGNAL_ILL  4
#define SIGNAL_TRAP 5
#define SIGNAL_ABRT 6
#define SIGNAL_BUS  7
#define SIGNAL_FPE  8
#define SIGNAL_KILL 9
#define SIGNAL_USR1 10
#define SIGNAL_SEGV 11
#define SIGNAL_USR2 12
#define SIGNAL_PIPE 13
#define SIGNAL_ALRM 14
#define SIGNAL_TERM 15
#define SIGNAL_CHLD 17
#define SIGNAL_CONT 18
#define SIGNAL_STOP 19
#define SIGNAL_TSTP 20
#define SIGNAL_TTIN 21
#define SIGNAL_TTOU 22

#define SIGNAL_COUNT 32

#ifndef __ASSEMBLY__

/* Function pointer type for signal handlers */
typedef void (*signal_handler_t)(int);

    /* Special handler values */
    #define SIGNAL_DFL ((signal_handler_t)0)    /* Default action */
    #define SIGNAL_IGN ((signal_handler_t)1)    /* Ignore signal */
    #define SIGNAL_ERR ((signal_handler_t) - 1) /* Error return */

#endif /* __ASSEMBLY__ */

#endif /* PERSPICUA_UAPI_SIGNALS_H */
