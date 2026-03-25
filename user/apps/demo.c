/*
 * demo.c - Workload generator for demonstrating ptop.
 *
 * Forks several worker processes, each doing something different:
 *
 *   allocator  — repeatedly malloc/free chunks of increasing size
 *   sleeper    — mostly sleeps, wakes briefly to touch memory
 *   pipe_ping  — writes through a pipe in a tight loop
 *   pipe_pong  — reads from the pipe and echoes back
 *   grandchild — forked by allocator, runs briefly then exits (shows
 *                zombie state momentarily before parent waitpid's it)
 *
 * The parent waits for all children and exits cleanly.
 *
 * Build alongside ptop.c and run demo in one terminal, ptop in another
 * (or just run ptop — it will see demo's children in /proc).
 */

#include "syscall.h"
#include "stdlib.h"
#include "types.h"

/* ── tiny helpers (no printf, just sys_write) ───────────────────── */

static size_t _strlen(const char* s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static void print(const char* s)
{
    sys_write(1, s, _strlen(s));
}

static void print_int(const char* prefix, int v, const char* suffix)
{
    char tmp[32];
    int neg = 0;
    if (v < 0)
    {
        neg = 1;
        v = -v;
    }
    int i = 30;
    tmp[31] = '\0';
    tmp[i] = '\0';
    do
    {
        tmp[--i] = '0' + (v % 10);
        v /= 10;
    } while (v);
    if (neg)
        tmp[--i] = '-';
    print(prefix);
    print(&tmp[i]);
    print(suffix);
}

/* ── memory helpers ─────────────────────────────────────────────── */

/*
 * Fill a block with a simple pattern so the compiler can't optimise
 * the allocation away.
 */
static void touch_memory(volatile char* p, size_t sz, char val)
{
    for (size_t i = 0; i < sz; i++)
        p[i] = val + (char)(i & 0x7f);
}

static char checksum(volatile char* p, size_t sz)
{
    char c = 0;
    for (size_t i = 0; i < sz; i++)
        c ^= p[i];
    return c;
}

/* ── worker: allocator ──────────────────────────────────────────── */
/*
 * Allocates a series of buffers of growing size, touches them,
 * occasionally forks a short-lived grandchild (to show transient
 * processes in ptop), then frees and repeats.
 *
 * Allocation sizes: 4 kB → 8 kB → 16 kB → 32 kB → 64 kB, cycling.
 */
#define ALLOC_STEPS 5
static const size_t alloc_sizes[ALLOC_STEPS] = {
    4 * 1024,
    8 * 1024,
    16 * 1024,
    32 * 1024,
    64 * 1024,
};

static void worker_allocator(void)
{
    print("[allocator] started\n");

    int grandchild_counter = 0;

    for (int cycle = 0; cycle < 40; cycle++)
    {
        for (int s = 0; s < ALLOC_STEPS; s++)
        {
            size_t sz = alloc_sizes[s];
            char* buf = (char*)malloc(sz);
            if (!buf)
            {
                print("[allocator] malloc failed\n");
                sys_sleep(200);
                continue;
            }

            touch_memory((volatile char*)buf, sz, (char)(cycle + s));
            char cs = checksum((volatile char*)buf, sz);
            (void)cs; /* suppress unused warning */

            /* every 3rd allocation in cycle 0,5,10,... spawn a grandchild */
            if (s == 2 && (cycle % 5) == 0)
            {
                grandchild_counter++;
                int gc_pid = sys_fork();
                if (gc_pid == 0)
                {
                    /* grandchild: allocate a small buffer, sleep, exit */
                    char* gc_buf = (char*)malloc(2048);
                    if (gc_buf)
                    {
                        touch_memory((volatile char*)gc_buf, 2048, 0xAB);
                        sys_sleep(800);
                        free(gc_buf);
                    }
                    print("[grandchild] exiting\n");
                    sys_exit(0);
                }
                else if (gc_pid > 0)
                {
                    print_int("[allocator] forked grandchild pid=", gc_pid, "\n");
                    /* don't block — let it show as running in ptop,
                       collect it after the inner loop */
                }
            }

            free(buf);
            sys_sleep(150);
        }

        /* reap any finished grandchildren without blocking */
        int status = 0;
        int reaped;
        do
        {
            reaped = sys_waitpid(-1, &status);
        } while (reaped > 0);

        sys_sleep(100);
    }

    print("[allocator] done\n");
    sys_exit(0);
}

/* ── worker: sleeper ────────────────────────────────────────────── */
/*
 * Simulates a low-activity daemon: sleeps most of the time, wakes up
 * briefly to allocate and immediately free a small buffer.
 */
static void worker_sleeper(void)
{
    print("[sleeper] started\n");

    for (int i = 0; i < 30; i++)
    {
        sys_sleep(1200);

        char* buf = (char*)malloc(1024);
        if (buf)
        {
            touch_memory((volatile char*)buf, 1024, (char)i);
            sys_sleep(50);
            free(buf);
        }
    }

    print("[sleeper] done\n");
    sys_exit(0);
}

/* ── worker: pipe_ping ──────────────────────────────────────────── */
/*
 * Writes a small message down the pipe every 300 ms, 60 times.
 * Visible in /proc/<pid>/fd/ as open file descriptors.
 */
static void worker_pipe_ping(int write_fd)
{
    print("[pipe_ping] started\n");

    const char* msg = "PING";
    int msg_len = 4;

    for (int i = 0; i < 60; i++)
    {
        sys_write(write_fd, msg, msg_len);
        sys_sleep(300);
    }

    sys_close(write_fd);
    print("[pipe_ping] done\n");
    sys_exit(0);
}

/* ── worker: pipe_pong ──────────────────────────────────────────── */
/*
 * Reads from the pipe, counts messages, allocates a small accumulator
 * buffer to show non-trivial VmSize.
 */
static void worker_pipe_pong(int read_fd)
{
    print("[pipe_pong] started\n");

    /* accumulator — grows by 512 bytes every 10 messages received */
    size_t acc_size = 512;
    char* acc = (char*)malloc(acc_size);
    int acc_idx = 0;
    int msgs = 0;

    char rbuf[8];
    for (;;)
    {
        int n = sys_read(read_fd, rbuf, sizeof(rbuf) - 1);
        if (n <= 0)
            break; /* pipe closed / EOF */
        rbuf[n] = '\0';
        msgs++;

        /* write received byte into accumulator, grow if needed */
        if ((size_t)acc_idx >= acc_size - 1)
        {
            free(acc);
            acc_size += 512;
            acc = (char*)malloc(acc_size);
            acc_idx = 0;
        }
        if (acc)
            acc[acc_idx++] = rbuf[0];
    }

    if (acc)
        free(acc);
    sys_close(read_fd);
    print_int("[pipe_pong] received msgs=", msgs, "\n");
    print("[pipe_pong] done\n");
    sys_exit(0);
}

/* ── worker: memory_hog ─────────────────────────────────────────── */
/*
 * Allocates a large region up front, keeps it alive for a while so
 * ptop shows a noticeably high VmSize, then frees it and exits.
 */
#define HOG_SIZE (128 * 1024) /* 128 kB */

static void worker_memory_hog(void)
{
    print("[mem_hog] started\n");

    char* big = (char*)malloc(HOG_SIZE);
    if (!big)
    {
        print("[mem_hog] malloc failed\n");
        sys_exit(1);
    }

    /* touch every page so it's actually resident */
    touch_memory((volatile char*)big, HOG_SIZE, 0x55);
    print("[mem_hog] allocated 128 kB, holding for 15s\n");

    /* hold for 15 seconds so ptop can show the VmSize */
    for (int i = 0; i < 15; i++)
    {
        /* re-touch a stripe each second so it looks "active" */
        size_t stripe = (HOG_SIZE / 15) * i;
        touch_memory((volatile char*)big + stripe, HOG_SIZE / 15, (char)i);
        sys_sleep(1000);
    }

    free(big);
    print("[mem_hog] freed, exiting\n");
    sys_exit(0);
}

/* ── main ───────────────────────────────────────────────────────── */
int demo_main(void)
{
    print("[demo] starting workload generator\n");

    /* create the pipe before forking ping/pong */
    int pipefd[2];
    if (sys_pipe(pipefd) < 0)
    {
        print("[demo] pipe() failed\n");
        sys_exit(1);
    }
    int pipe_read = pipefd[0];
    int pipe_write = pipefd[1];

    /* ── fork all workers ── */

    int pid_allocator = sys_fork();
    if (pid_allocator == 0)
        worker_allocator(); /* never returns */

    int pid_sleeper = sys_fork();
    if (pid_sleeper == 0)
        worker_sleeper(); /* never returns */

    int pid_hog = sys_fork();
    if (pid_hog == 0)
        worker_memory_hog(); /* never returns */

    int pid_ping = sys_fork();
    if (pid_ping == 0)
    {
        sys_close(pipe_read);         /* ping only needs write end */
        worker_pipe_ping(pipe_write); /* never returns */
    }

    int pid_pong = sys_fork();
    if (pid_pong == 0)
    {
        sys_close(pipe_write);       /* pong only needs read end */
        worker_pipe_pong(pipe_read); /* never returns */
    }

    /* parent closes both pipe ends — ping and pong own them now */
    sys_close(pipe_read);
    sys_close(pipe_write);

    print_int("[demo] allocator pid=", pid_allocator, "\n");
    print_int("[demo] sleeper   pid=", pid_sleeper, "\n");
    print_int("[demo] mem_hog   pid=", pid_hog, "\n");
    print_int("[demo] pipe_ping pid=", pid_ping, "\n");
    print_int("[demo] pipe_pong pid=", pid_pong, "\n");
    print("[demo] all workers launched, waiting...\n");

    /* ── wait for all direct children ── */
    int pids[4] = {pid_allocator, pid_sleeper, pid_hog, pid_ping};
    for (int i = 0; i < 4; i++)
    {
        int status = 0;
        sys_waitpid(pids[i], &status);
        print_int("[demo] child exited pid=", pids[i], "\n");
    }

    /* pipe_pong exits on its own when ping closes the write end;
       wait for it too */
    {
        int status = 0;
        sys_waitpid(pid_pong, &status);
        print_int("[demo] pipe_pong exited pid=", pid_pong, "\n");
    }

    print("[demo] all done, exiting\n");
    sys_exit(0);
    return 0;
}
