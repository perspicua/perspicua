/*
 * syscall.c - POSIX entry points, each one trap into the kernel.
 */

#include "syscall.h"
#include "errno.h"

#include "uapi/syscalls.h"
#include "uapi/mman.h"

/*
 * The syscall ABI: arguments in x0-x5, number in x8, result in x0. Naming the
 * registers with register-asm variables lets the compiler place the arguments
 * itself, so each wrapper below is a call rather than a copy of the same
 * hand-written "mov x0, ...; svc #0" block.
 *
 * Six arguments is the ceiling because x5 is the last argument register; only
 * mmap reaches it.
 */
#define SVC_CLOBBERS "memory", "cc"

static inline long __syscall0(long n)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0");
    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : SVC_CLOBBERS);
    return x0;
}

static inline long __syscall1(long n, long a)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : SVC_CLOBBERS);
    return x0;
}

static inline long __syscall2(long n, long a, long b)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : SVC_CLOBBERS);
    return x0;
}

static inline long __syscall3(long n, long a, long b, long c)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : SVC_CLOBBERS);
    return x0;
}

static inline long __syscall4(long n, long a, long b, long c, long d)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : SVC_CLOBBERS);
    return x0;
}

static inline long __syscall5(long n, long a, long b, long c, long d, long e)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
                     : SVC_CLOBBERS);
    return x0;
}

static inline long __syscall6(long n, long a, long b, long c, long d, long e, long f)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                     : SVC_CLOBBERS);
    return x0;
}

/*
 * The kernel returns -errno, so recovering the number is a negation. Nothing
 * translates between two vocabularies any more: both sides name the same
 * values out of uapi/errno.h.
 */
static inline int __set_errno_ret(long res)
{
    errno = (int)-res;
    return -1;
}

static inline int __syscall_ret(long res)
{
    return res < 0 ? __set_errno_ret(res) : (int)res;
}

static inline off_t __syscall_off_ret(long res)
{
    if (res < 0) {
        errno = (int)-res;
        return (off_t)-1;
    }
    return (off_t)res;
}

static inline void *__syscall_mmap_ret(long res)
{
    if (res < 0) {
        errno = (int)-res;
        return MAP_FAILED;
    }
    return (void *)res;
}

// Process control

__attribute__((noreturn)) void _exit(int status)
{
    __syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

int fork(void)
{
    return __syscall_ret(__syscall0(SYS_FORK));
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    return __syscall_ret(__syscall3(SYS_EXEC, (long)path, (long)argv, (long)envp));
}

int waitpid(int pid, int *status, int options)
{
    return __syscall_ret(__syscall3(SYS_WAITPID, pid, (long)status, options));
}

int getpid(void)
{
    return __syscall_ret(__syscall0(SYS_GETPID));
}

int getppid(void)
{
    return __syscall_ret(__syscall0(SYS_GETPPID));
}

int sched_yield(void)
{
    return __syscall_ret(__syscall0(SYS_YIELD));
}

int setpgid(int pid, int pgid)
{
    return __syscall_ret(__syscall2(SYS_SETPGID, pid, pgid));
}

int getpgid(int pid)
{
    return __syscall_ret(__syscall1(SYS_GETPGID, pid));
}

int setsid(void)
{
    return __syscall_ret(__syscall0(SYS_SETSID));
}

int getsid(int pid)
{
    return __syscall_ret(__syscall1(SYS_GETSID, pid));
}

int tcsetpgrp(int fd, int pgid)
{
    return __syscall_ret(__syscall2(SYS_TCSETPGRP, fd, pgid));
}

int tcgetpgrp(int fd)
{
    return __syscall_ret(__syscall1(SYS_TCGETPGRP, fd));
}

// Filesystem and I/O

int open(const char *path, int flags)
{
    return __syscall_ret(__syscall2(SYS_OPEN, (long)path, flags));
}

int close(int fd)
{
    return __syscall_ret(__syscall1(SYS_CLOSE, fd));
}

int read(int fd, void *buf, size_t len)
{
    return __syscall_ret(__syscall3(SYS_READ, fd, (long)buf, (long)len));
}

int write(int fd, const char *buf, size_t len)
{
    return __syscall_ret(__syscall3(SYS_WRITE, fd, (long)buf, (long)len));
}

int pread(int fd, void *buf, size_t count, off_t offset)
{
    return __syscall_ret(__syscall4(SYS_PREAD, fd, (long)buf, (long)count, (long)offset));
}

int pwrite(int fd, const char *buf, size_t len, off_t offset)
{
    return __syscall_ret(__syscall4(SYS_PWRITE, fd, (long)buf, (long)len, (long)offset));
}

int getdents(int fd, void *buf, size_t count)
{
    return __syscall_ret(__syscall3(SYS_GETDENTS, fd, (long)buf, (long)count));
}

int pipe(int pipefd[2])
{
    return __syscall_ret(__syscall1(SYS_PIPE, (long)pipefd));
}

int dup2(int oldfd, int newfd)
{
    return __syscall_ret(__syscall2(SYS_DUP2, oldfd, newfd));
}

int chdir(const char *path)
{
    return __syscall_ret(__syscall1(SYS_CHDIR, (long)path));
}

/*
 * POSIX reports success by handing back the buffer, not by returning 0, so a
 * caller can write `if (getcwd(...))`.
 */
char *getcwd(char *buf, size_t size)
{
    long res = __syscall2(SYS_GETCWD, (long)buf, (long)size);
    if (res < 0) {
        errno = (int)-res;
        return NULL;
    }
    return buf;
}

int stat(const char *path, struct stat *buf)
{
    return __syscall_ret(__syscall2(SYS_STAT, (long)path, (long)buf));
}

int fstat(int fd, struct stat *buf)
{
    return __syscall_ret(__syscall2(SYS_FSTAT, fd, (long)buf));
}

int truncate(const char *path, off_t length)
{
    return __syscall_ret(__syscall2(SYS_TRUNCATE, (long)path, (long)length));
}

int ftruncate(int fd, off_t length)
{
    return __syscall_ret(__syscall2(SYS_FTRUNCATE, fd, (long)length));
}

off_t lseek(int fd, off_t offset, int whence)
{
    return __syscall_off_ret(__syscall3(SYS_LSEEK, fd, (long)offset, whence));
}

int mkdir(const char *path, int mode)
{
    return __syscall_ret(__syscall2(SYS_MKDIR, (long)path, mode));
}

int rmdir(const char *path)
{
    return __syscall_ret(__syscall1(SYS_RMDIR, (long)path));
}

int unlink(const char *path)
{
    return __syscall_ret(__syscall1(SYS_UNLINK, (long)path));
}

int rename(const char *oldpath, const char *newpath)
{
    return __syscall_ret(__syscall2(SYS_RENAME, (long)oldpath, (long)newpath));
}

int fcntl(int fd, int cmd, int arg)
{
    return __syscall_ret(__syscall3(SYS_FCNTL, fd, cmd, arg));
}

int sync(void)
{
    return __syscall_ret(__syscall0(SYS_SYNC));
}

int fsync(int fd)
{
    return __syscall_ret(__syscall1(SYS_FSYNC, fd));
}

// Memory management

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    return __syscall_mmap_ret(
        __syscall6(SYS_MMAP, (long)addr, (long)length, prot, flags, fd, (long)offset));
}

// Signal handling

void __sigreturn(void)
{
    __syscall0(SYS_SIGRETURN);
}

extern void __sigrestorer(void);

/*
 * The kernel needs a return trampoline to resume through, and a caller that
 * built its own sigaction by hand will not have supplied one; fill it in here
 * rather than rejecting the call.
 */
int sigaction(int sig, const struct sigaction *act, struct sigaction *oact)
{
    struct sigaction kact;
    const struct sigaction *pact = act;

    if (act != NULL) {
        kact = *act;
        if (!(kact.sa_flags & SA_RESTORER) || kact.sa_restorer == NULL) {
            kact.sa_flags |= SA_RESTORER;
            kact.sa_restorer = __sigrestorer;
        }
        pact = &kact;
    }

    return __syscall_ret(__syscall3(SYS_SIGACTION, sig, (long)pact, (long)oact));
}

sighandler_t signal(int sig, sighandler_t handler)
{
    struct sigaction act = {0}, oact = {0};
    act.sa_handler = handler;

    if (sigaction(sig, &act, &oact) < 0) {
        return SIG_ERR;
    }
    return oact.sa_handler;
}

int kill(int pid, int sig)
{
    return __syscall_ret(__syscall2(SYS_KILL, pid, sig));
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oset)
{
    return __syscall_ret(__syscall3(SYS_SIGPROCMASK, how, (long)set, (long)oset));
}

int sigpending(sigset_t *set)
{
    return __syscall_ret(__syscall1(SYS_SIGPENDING, (long)set));
}

int sigaltstack(const stack_t *ss, stack_t *oss)
{
    return __syscall_ret(__syscall2(SYS_SIGALTSTACK, (long)ss, (long)oss));
}

int sigsuspend(const sigset_t *mask)
{
    return __syscall_ret(__syscall1(SYS_SIGSUSPEND, (long)mask));
}

// Time

int gettimeofday(struct timeval *tv, void *tz)
{
    return __syscall_ret(__syscall2(SYS_GETTIMEOFDAY, (long)tv, (long)tz));
}

int clock_gettime(clockid_t clk_id, struct timespec *tp)
{
    return __syscall_ret(__syscall2(SYS_CLOCK_GETTIME, (long)clk_id, (long)tp));
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    return __syscall_ret(__syscall2(SYS_NANOSLEEP, (long)req, (long)rem));
}

int usleep(useconds_t usec)
{
    struct timespec req = {.tv_sec = (time_t)(usec / 1000000u),
                           .tv_nsec = (long)(usec % 1000000u) * 1000};
    return nanosleep(&req, NULL);
}

unsigned int sleep(unsigned int seconds)
{
    struct timespec req = {.tv_sec = (time_t)seconds, .tv_nsec = 0}, rem = {0};
    if (nanosleep(&req, &rem) == 0) {
        return 0;
    }
    return (unsigned int)rem.tv_sec; // POSIX: the time left unslept
}
