/*
 * signal.h - POSIX signal dispositions and delivery.
 */

#ifndef PERSPICUA_LIBC_SIGNAL_H
#define PERSPICUA_LIBC_SIGNAL_H

#include "uapi/signals.h"

sighandler_t signal(int sig, sighandler_t handler);
int kill(int pid, int sig);
int sigaction(int sig, const struct sigaction *act, struct sigaction *oact);
int sigprocmask(int how, const sigset_t *set, sigset_t *oset);
int sigpending(sigset_t *set);
int sigsuspend(const sigset_t *mask);

// Returns from a handler through the kernel; the restorer calls it, not you.
void __sigreturn(void);

#endif // PERSPICUA_LIBC_SIGNAL_H
