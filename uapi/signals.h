/*
 * signals.h - Signal numbers, dispositions, and sigaction, shared with userspace.
 */

#ifndef PERSPICUA_UAPI_SIGNALS_H
#define PERSPICUA_UAPI_SIGNALS_H

#include <stddef.h>
#include <stdint.h>

// Signal Numbers

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGIOT    SIGABRT
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPWR    30
#define SIGSYS    31

#define NSIG 32

#ifndef __ASSEMBLY__

// Types and Constants

typedef uint32_t sigset_t;
typedef void (*sighandler_t)(int);

    #define SIG_DFL ((sighandler_t)0)    // Default action
    #define SIG_IGN ((sighandler_t)1)    // Ignore signal
    #define SIG_ERR ((sighandler_t) - 1) // Error return

    // sigaction flags
    #define SA_NOCLDSTOP 0x00000001
    #define SA_NOCLDWAIT 0x00000002
    #define SA_SIGINFO   0x00000004
    #define SA_ONSTACK   0x08000000
    #define SA_RESTART   0x10000000
    #define SA_NODEFER   0x40000000
    #define SA_RESETHAND 0x80000000
    #define SA_RESTORER  0x04000000

    // sigprocmask "how" values
    #define SIG_BLOCK   0
    #define SIG_UNBLOCK 1
    #define SIG_SETMASK 2

// Data Structures

/*
 * struct sigaction - Defines the action to be taken upon signal delivery.
 */
struct sigaction {
    sighandler_t sa_handler;
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

    // sigaltstack ss_flags
    #define SS_ONSTACK 1
    #define SS_DISABLE 2

    // A frame plus SIGNAL_STACK_GUARD rounded up; SIGSTKSZ leaves room to work.
    #define MINSIGSTKSZ 2048
    #define SIGSTKSZ    8192

typedef struct {
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
} stack_t;

#endif // __ASSEMBLY__

#endif // PERSPICUA_UAPI_SIGNALS_H
