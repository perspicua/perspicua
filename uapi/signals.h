/*
 * signals.h - Public API for signal numbers and handling structures.
 *
 * This header defines the standard signal numbers and action structures
 * shared between the kernel and userspace.
 */

#ifndef PERSPICUA_UAPI_SIGNALS_H
#define PERSPICUA_UAPI_SIGNALS_H

#include "types.h"

/* --- Signal Numbers --- */

#define SIGNAL_HUP    1
#define SIGNAL_INT    2
#define SIGNAL_QUIT   3
#define SIGNAL_ILL    4
#define SIGNAL_TRAP   5
#define SIGNAL_ABRT   6
#define SIGNAL_IOT    SIGNAL_ABRT
#define SIGNAL_BUS    7
#define SIGNAL_FPE    8
#define SIGNAL_KILL   9
#define SIGNAL_USR1   10
#define SIGNAL_SEGV   11
#define SIGNAL_USR2   12
#define SIGNAL_PIPE   13
#define SIGNAL_ALRM   14
#define SIGNAL_TERM   15
#define SIGNAL_STKFLT 16
#define SIGNAL_CHLD   17
#define SIGNAL_CONT   18
#define SIGNAL_STOP   19
#define SIGNAL_TSTP   20
#define SIGNAL_TTIN   21
#define SIGNAL_TTOU   22
#define SIGNAL_URG    23
#define SIGNAL_XCPU   24
#define SIGNAL_XFSZ   25
#define SIGNAL_VTALRM 26
#define SIGNAL_PROF   27
#define SIGNAL_WINCH  28
#define SIGNAL_IO     29
#define SIGNAL_PWR    30
#define SIGNAL_SYS    31

#define SIGNAL_COUNT 32

#ifndef __ASSEMBLY__

/* --- Types and Constants --- */

typedef uint32_t sigset_t;
typedef void (*signal_handler_t)(int);

    #define SIGNAL_DFL ((signal_handler_t)0)    /* Default action */
    #define SIGNAL_IGN ((signal_handler_t)1)    /* Ignore signal */
    #define SIGNAL_ERR ((signal_handler_t) - 1) /* Error return */

    /* sigaction flags */
    #define SA_NOCLDSTOP 0x00000001
    #define SA_NOCLDWAIT 0x00000002
    #define SA_SIGINFO   0x00000004
    #define SA_ONSTACK   0x08000000
    #define SA_RESTART   0x10000000
    #define SA_NODEFER   0x40000000
    #define SA_RESETHAND 0x80000000
    #define SA_RESTORER  0x04000000

    /* sigprocmask "how" values */
    #define SIG_BLOCK   0
    #define SIG_UNBLOCK 1
    #define SIG_SETMASK 2

/* --- Data Structures --- */

/*
 * struct sigaction - Defines the action to be taken upon signal delivery.
 */
struct sigaction {
    signal_handler_t sa_handler;
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

#endif /* __ASSEMBLY__ */

#endif /* PERSPICUA_UAPI_SIGNALS_H */
