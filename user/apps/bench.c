/*
 * bench.c - Microbenchmarks for the kernel's hot paths.
 *
 * `bench` runs every test, `bench <name>...` only those named. Each test
 * doubles its run count until one run takes BUDGET_MS, and reports that run.
 */

#include <stddef.h>

#include "fcntl.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/mman.h"
#include "sys/wait.h"
#include "time.h"
#include "unistd.h"

#define BUDGET_MS 300
#define FILE_MB   1
#define MB        (1024 * 1024)
#define CHUNK     (64 * 1024)
#define TMP_FILE  "/bench.tmp"
#define SELF      "/bin/bench.elf"

struct bench {
    const char *name;
    const char *what;
    int (*setup)(void);
    int (*run)(long n);
    void (*teardown)(void);
    long bytes; // per run, for a MB/s rate; 0 reports runs per second
    long max_n; // caps the run count where each run costs memory or card writes
};

static unsigned long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000 + (unsigned long)ts.tv_nsec / 1000000;
}

// syscall

static int run_syscall(long n)
{
    for (long i = 0; i < n; i++) {
        getpid();
    }
    return 0;
}

// pipe: a child echoes each byte back, so one run is two switches between processes

static int to_child[2], from_child[2];
static int echo_pid = -1;

static int setup_pipe(void)
{
    if (pipe(to_child) != 0 || pipe(from_child) != 0) {
        return -1;
    }
    echo_pid = fork();
    if (echo_pid < 0) {
        return -1;
    }
    if (echo_pid == 0) {
        close(to_child[1]);
        close(from_child[0]);
        char c;
        while (read(to_child[0], &c, 1) == 1) {
            write(from_child[1], &c, 1);
        }
        _exit(0);
    }
    close(to_child[0]);
    close(from_child[1]);
    return 0;
}

static int run_pipe(long n)
{
    char c = 'x';
    for (long i = 0; i < n; i++) {
        if (write(to_child[1], &c, 1) != 1 || read(from_child[0], &c, 1) != 1) {
            return -1;
        }
    }
    return 0;
}

static void teardown_pipe(void)
{
    close(to_child[1]);
    close(from_child[0]);
    if (echo_pid > 0) {
        int status;
        waitpid(echo_pid, &status, 0);
    }
}

// fork and exec

static int run_fork(long n)
{
    for (long i = 0; i < n; i++) {
        int pid = fork();
        if (pid < 0) {
            return -1;
        }
        if (pid == 0) {
            _exit(0);
        }
        int status;
        waitpid(pid, &status, 0);
    }
    return 0;
}

static int run_exec(long n)
{
    for (long i = 0; i < n; i++) {
        int pid = fork();
        if (pid < 0) {
            return -1;
        }
        if (pid == 0) {
            char *argv[] = {SELF, "--noop", NULL};
            execve(SELF, argv, environ);
            _exit(127);
        }
        int status;
        waitpid(pid, &status, 0);
        if (status != 0) {
            return -1;
        }
    }
    return 0;
}

// mmap: one run is one page, mapped and then touched

static int run_mmap(long n)
{
    char *p =
        mmap(NULL, (size_t)n * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return -1;
    }
    for (long i = 0; i < n; i++) {
        p[i * 4096] = 1;
    }
    return 0;
}

// memcpy

static char copy_src[MB], copy_dst[MB];

static int run_memcpy(long n)
{
    for (long i = 0; i < n; i++) {
        copy_src[i % MB] = (char)i;
        memcpy(copy_dst, copy_src, MB);
    }
    return copy_dst[0] == copy_src[0] ? 0 : -1;
}

// write and read: FILE_MB through the file system

static char io_buf[CHUNK];

static int write_file(int sync)
{
    int fd = open(TMP_FILE, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        return -1;
    }
    for (int done = 0; done < FILE_MB * MB; done += CHUNK) {
        if (write(fd, io_buf, CHUNK) != CHUNK) {
            close(fd);
            return -1;
        }
    }
    if (sync && fsync(fd) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int run_write(long n)
{
    for (long i = 0; i < n; i++) {
        if (write_file(1) != 0) {
            return -1;
        }
    }
    return 0;
}

static void remove_file(void)
{
    unlink(TMP_FILE);
}

static int setup_read(void)
{
    return write_file(1);
}

static int run_read(long n)
{
    for (long i = 0; i < n; i++) {
        int fd = open(TMP_FILE, O_RDONLY);
        if (fd < 0) {
            return -1;
        }
        int total = 0;
        int got;
        while ((got = read(fd, io_buf, CHUNK)) > 0) {
            total += got;
        }
        close(fd);
        if (total != FILE_MB * MB) {
            return -1;
        }
    }
    return 0;
}

// sleep

static int run_sleep(long n)
{
    struct timespec req = {0, 1000000};
    for (long i = 0; i < n; i++) {
        nanosleep(&req, NULL);
    }
    return 0;
}

static const struct bench benches[] = {
    {"syscall", "getpid round trip", NULL, run_syscall, NULL, 0, 0},
    {"pipe", "1-byte ping-pong between two processes", setup_pipe, run_pipe, teardown_pipe, 0, 0},
    {"fork", "fork, child exits, waitpid", NULL, run_fork, NULL, 0, 0},
    {"exec", "fork, exec a tiny program, waitpid", NULL, run_exec, NULL, 0, 0},
    {"mmap", "map and touch one anonymous page", NULL, run_mmap, NULL, 0, 8192},
    {"memcpy", "copy 1 MB", NULL, run_memcpy, NULL, MB, 0},
    {"write", "write 1 MB to the card and fsync", NULL, run_write, remove_file, MB, 8},
    {"read", "read a cached 1 MB file", setup_read, run_read, remove_file, MB, 0},
    {"sleep", "nanosleep asked for 1 ms", NULL, run_sleep, NULL, 0, 64},
};

#define NBENCH (sizeof(benches) / sizeof(benches[0]))

// Formats a time in nanoseconds with three significant digits.
static void fmt_ns(char *out, size_t size, unsigned long ns)
{
    static const char *units[] = {"ns", "us", "ms", "s"};
    unsigned long scale = 1;
    int u = 0;
    while (u < 3 && ns >= scale * 1000) {
        scale *= 1000;
        u++;
    }
    unsigned long whole = ns / scale;
    unsigned long frac = (ns % scale) * 100 / scale;
    if (u == 0) {
        snprintf(out, size, "%lu %s", whole, units[u]);
    } else {
        snprintf(out, size, "%lu.%02lu %s", whole, frac, units[u]);
    }
}

static void fmt_rate(char *out, size_t size, const struct bench *b, long n, unsigned long ms)
{
    if (ms == 0) {
        snprintf(out, size, "-");
        return;
    }
    if (b->bytes) {
        unsigned long kb = (unsigned long)(b->bytes / 1024) * (unsigned long)n;
        unsigned long mb10 = kb * 1000 / ms * 10 / 1024; // tenths of MB/s
        snprintf(out, size, "%lu.%lu MB/s", mb10 / 10, mb10 % 10);
        return;
    }
    unsigned long per_s = (unsigned long)n * 1000 / ms;
    if (per_s >= 1000000) {
        snprintf(out, size, "%lu.%02lu M/s", per_s / 1000000, per_s % 1000000 / 10000);
    } else if (per_s >= 1000) {
        snprintf(out, size, "%lu.%02lu k/s", per_s / 1000, per_s % 1000 / 10);
    } else {
        snprintf(out, size, "%lu /s", per_s);
    }
}

static void measure(const struct bench *b)
{
    if (b->setup && b->setup() != 0) {
        printf("  %-8s setup failed\n", b->name);
        return;
    }

    long n = 1;
    unsigned long ms;
    for (;;) {
        unsigned long t0 = now_ms();
        if (b->run(n) != 0) {
            printf("  %-8s failed after %ld runs\n", b->name, n);
            if (b->teardown) {
                b->teardown();
            }
            return;
        }
        ms = now_ms() - t0;
        if (ms >= BUDGET_MS || (b->max_n && n >= b->max_n)) {
            break;
        }
        // Once a run is long enough to time, jump straight to the budget.
        long next = ms >= 20 ? (long)((unsigned long)n * BUDGET_MS / ms) + 1 : n * 4;
        if (next < n * 2) {
            next = n * 2;
        }
        n = b->max_n && next > b->max_n ? b->max_n : next;
    }

    if (b->teardown) {
        b->teardown();
    }

    char per[24], rate[24];
    fmt_ns(per, sizeof(per), ms * 1000000 / (unsigned long)n);
    fmt_rate(rate, sizeof(rate), b, n, ms);
    printf("  %-8s %9ld %11s %12s   %s\n", b->name, n, per, rate, b->what);
}

// Each test runs in its own process: fork costs grow with the memory a parent has mapped.
static void measure_isolated(const struct bench *b)
{
    int pid = fork();
    if (pid == 0) {
        measure(b);
        _exit(0);
    }
    if (pid < 0) {
        printf("  %-8s could not fork\n", b->name);
        return;
    }
    int status;
    waitpid(pid, &status, 0);
}

static const struct bench *find(const char *name)
{
    for (size_t i = 0; i < NBENCH; i++) {
        if (strcmp(benches[i].name, name) == 0) {
            return &benches[i];
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--noop") == 0) {
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if (!find(argv[i])) {
            printf("bench: no test named '%s'; tests are:", argv[i]);
            for (size_t k = 0; k < NBENCH; k++) {
                printf(" %s", benches[k].name);
            }
            printf("\n");
            return 1;
        }
    }

    unsigned long start = now_ms();
    printf("  %-8s %9s %11s %12s   %s\n", "test", "runs", "per run", "rate", "what");
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            measure_isolated(find(argv[i]));
        }
    } else {
        for (size_t i = 0; i < NBENCH; i++) {
            measure_isolated(&benches[i]);
        }
    }
    printf("bench: done in %lu ms (clock resolution 1 ms)\n", now_ms() - start);
    return 0;
}
