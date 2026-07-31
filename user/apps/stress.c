/*
 * stress.c - Kernel stress and invariant checker.
 *
 * Every test here verifies something the kernel could get wrong and reports a
 * verdict, so a clean run means "no defect found" rather than "nothing
 * crashed". Three properties make that worth something:
 *
 *   - Content is checked with a key-derived pattern, not a constant fill. A
 *     memset cannot tell a page the kernel never wrote from one another
 *     process wrote, and both are bugs.
 *   - The PRNG is seeded and the seed is printed, so a failing run replays
 *     exactly with -s.
 *   - Resource tests run several waves and compare high-water marks. A leak
 *     shows up as a later wave reaching fewer than the first.
 *
 * Children never print: concurrent writes to one UART interleave into garbage
 * and the output stops being evidence. They report through their exit status
 * instead, which is why the verdict codes below exist.
 *
 * Each test prints its name before it runs, so a hang names the test that hung.
 */

#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "uapi/mman.h"
#include "signals.h"
#include "wait.h"

#define PAGE_SIZE 4096

/* Bounds a wave of children so a run cannot wedge the system it is testing. */
#define DEFAULT_MAX_KIDS 96

/* ------------------------------------------------------------------------ */
/* Verdicts                                                                  */
/* ------------------------------------------------------------------------ */

enum kid_verdict {
    KID_OK = 0,
    KID_SETUP,       /* could not set itself up; inconclusive, not a pass */
    KID_PARENT_DATA, /* did not see the parent's snapshot */
    KID_OWN_DATA,    /* its own store did not read back */
    KID_ZERO_PAGE,   /* a fresh anonymous page was not zero-filled */
    KID_PPID,        /* not reparented after its parent exited */
    KID_STREAM,      /* pipe content or length wrong */
    KID_ARGV,        /* exec did not deliver argv intact */
    KID_FORK,        /* a nested fork failed unexpectedly */
    KID_SIGNAL,      /* signal not delivered, or delivered while blocked */
    KID_FILE,        /* file content read back wrong */
    KID_VERDICT_COUNT
};

static const char *verdict_name(int v)
{
    static const char *names[KID_VERDICT_COUNT] = {
        "ok",          "setup failed",   "parent data wrong", "own store lost",
        "page dirty",  "not reparented", "stream corrupt",    "argv corrupt",
        "fork failed", "signal lost",    "file corrupt",
    };
    if (v >= 0 && v < KID_VERDICT_COUNT) {
        return names[v];
    }
    return "unknown";
}

/* ------------------------------------------------------------------------ */
/* Reporting                                                                 */
/* ------------------------------------------------------------------------ */

static int g_checks;
static int g_failures;
static int g_verbose;

static void report_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void report_fail(const char *fmt, ...)
{
    va_list args;
    g_failures++;
    printf("    FAIL: ");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

#define CHECK(cond, ...)              \
    do {                              \
        g_checks++;                   \
        if (!(cond)) {                \
            report_fail(__VA_ARGS__); \
        }                             \
    } while (0)

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void note(const char *fmt, ...)
{
    va_list args;
    if (!g_verbose) {
        return;
    }
    printf("    ");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

/* ------------------------------------------------------------------------ */
/* Deterministic randomness and content patterns                             */
/* ------------------------------------------------------------------------ */

static uint64_t g_rng;

static uint32_t rnd(void)
{
    /* xorshift64*, so a printed seed reproduces the whole run */
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return (uint32_t)((g_rng * 2685821657736338717ULL) >> 32);
}

/*
 * Word i of a pattern depends on both the key and the offset, so a check
 * catches a page swapped for another key's page and a page shifted against
 * itself, not just a page of zeroes.
 */
static uint32_t pattern_word(uint32_t key, size_t index)
{
    uint32_t x = key + (uint32_t)index * 2654435761u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return x;
}

static void pattern_fill(void *buf, size_t len, uint32_t key)
{
    uint32_t *w = (uint32_t *)buf;
    for (size_t i = 0; i < len / sizeof(uint32_t); i++) {
        w[i] = pattern_word(key, i);
    }
}

/* Index of the first mismatching word, or -1 when the buffer matches. */
static long pattern_check(const void *buf, size_t len, uint32_t key)
{
    const uint32_t *w = (const uint32_t *)buf;
    for (size_t i = 0; i < len / sizeof(uint32_t); i++) {
        if (w[i] != pattern_word(key, i)) {
            return (long)i;
        }
    }
    return -1;
}

static void byte_pattern_fill(unsigned char *buf, size_t len, uint32_t key)
{
    for (size_t i = 0; i < len; i++) {
        buf[i] = (unsigned char)(pattern_word(key, i / 4) >> ((i % 4) * 8));
    }
}

static long byte_pattern_check(const unsigned char *buf, size_t len, uint32_t key)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != (unsigned char)(pattern_word(key, i / 4) >> ((i % 4) * 8))) {
            return (long)i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------ */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------ */

static void *map_anon(size_t len)
{
    void *p = sys_mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

/* Pipes and files are both free to satisfy a request partially. */
static int write_full(int fd, const unsigned char *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        int n = sys_write(fd, (const char *)buf + done, len - done);
        if (n <= 0) {
            return n < 0 ? n : (int)done;
        }
        done += (size_t)n;
    }
    return (int)done;
}

static int read_full(int fd, unsigned char *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        int n = sys_read(fd, buf + done, len - done);
        if (n <= 0) {
            return n < 0 ? n : (int)done;
        }
        done += (size_t)n;
    }
    return (int)done;
}

/* Generous: a wave member competing with 96 others still finishes far inside. */
#define REAP_DEADLINE_MS 8000

/*
 * waitpid with a deadline. A defect in a blocking kernel path shows up as a
 * child that never exits, and a stress tool that blocks on it forever reports
 * nothing at all -- the run just stops, which is the least useful outcome. Give
 * up instead, kill the child, and call it a failure.
 *
 * Returns the reaped pid, or -1 once the deadline has passed and been reported.
 */
static int wait_deadline(int pid, int *status, const char *what)
{
    for (int waited = 0; waited <= REAP_DEADLINE_MS; waited += 10) {
        int got = sys_waitpid(pid, status, WNOHANG);

        if (got != 0) {
            return got; /* reaped, or no such child */
        }
        sys_sleep(10);
    }

    report_fail("%s: pid %d never exited (blocked); killing it", what, pid);
    sys_kill(pid, SIGNAL_KILL);
    sys_waitpid(pid, status, 0);
    return -1;
}

/*
 * Collect a wave of children and fold their verdicts into one. Reaping by
 * explicit pid also checks that waitpid returns the child that was asked for
 * rather than whichever one happened to be ready.
 */
static int reap_wave(const int *pids, int n, const char *what)
{
    int worst = KID_OK;

    for (int i = 0; i < n; i++) {
        int status = -1;
        int got = wait_deadline(pids[i], &status, what);

        g_checks++;
        if (got != pids[i]) {
            if (got >= 0) {
                report_fail("%s: waitpid(%d) returned %d", what, pids[i], got);
            }
            /* A deadline miss has already reported itself. */
            if (worst < KID_SETUP) {
                worst = KID_SETUP;
            }
            continue;
        }
        g_checks++;
        if (status != KID_OK) {
            report_fail("%s: child %d reports %s (%d)", what, pids[i], verdict_name(status),
                        status);
            if (status > worst) {
                worst = status;
            }
        }
    }
    return worst;
}

/* ------------------------------------------------------------------------ */
/* cow - copy-on-write isolation under a fork storm                          */
/* ------------------------------------------------------------------------ */

/*
 * The invariant is symmetric and both halves have been broken before: a child
 * must see the parent's memory exactly as it stood at fork and never a later
 * write, and each side's own stores must read back. The failure this is built
 * to catch is a fault resolved for the wrong address, which leaves the page
 * read-only so the store is silently dropped -- registers advance, memory does
 * not. Yielding between write and re-read puts a context switch inside the
 * window where that happens.
 */
static void test_cow(int max_kids)
{
    const size_t pages = 16;
    const size_t len = pages * PAGE_SIZE;
    int kids = max_kids < 12 ? max_kids : 12;

    unsigned char *region = map_anon(len);
    if (!region) {
        CHECK(0, "cow: mmap of %zu bytes failed", len);
        return;
    }

    uint32_t snapshot_key = rnd();
    pattern_fill(region, len, snapshot_key);

    int *pids = malloc(sizeof(int) * (size_t)kids);
    if (!pids) {
        CHECK(0, "cow: out of memory");
        return;
    }

    int n = 0;
    for (int i = 0; i < kids; i++) {
        int pid = sys_fork();
        if (pid < 0) {
            break;
        }
        if (pid == 0) {
            uint32_t own_key = snapshot_key ^ (0x9E3779B9u * (uint32_t)(i + 1));

            if (pattern_check(region, len, snapshot_key) >= 0) {
                sys_exit(KID_PARENT_DATA);
            }
            for (size_t p = 0; p < pages; p++) {
                unsigned char *page = region + p * PAGE_SIZE;
                pattern_fill(page, PAGE_SIZE, own_key + (uint32_t)p);
                sys_yield();
                if (pattern_check(page, PAGE_SIZE, own_key + (uint32_t)p) >= 0) {
                    sys_exit(KID_OWN_DATA);
                }
            }
            /* A second pass catches a store that landed in a page later reclaimed. */
            for (size_t p = 0; p < pages; p++) {
                if (pattern_check(region + p * PAGE_SIZE, PAGE_SIZE, own_key + (uint32_t)p) >= 0) {
                    sys_exit(KID_OWN_DATA);
                }
            }
            sys_exit(KID_OK);
        }
        pids[n++] = pid;
    }

    CHECK(n > 0, "cow: could not fork any children");

    /*
     * Rewrite while the children run. Every child snapshotted the old key at
     * fork, so these writes must stay invisible to all of them -- and each one
     * faults the parent's side of the same shared pages at the same time.
     */
    for (int round = 0; round < 4; round++) {
        uint32_t key = snapshot_key ^ (0xA5A5A5A5u * (uint32_t)(round + 1));
        pattern_fill(region, len, key);
        sys_yield();
        long bad = pattern_check(region, len, key);
        if (bad >= 0) {
            CHECK(0, "cow: parent lost its own write at word %ld of round %d", bad, round);
            break;
        }
        g_checks++;
    }

    reap_wave(pids, n, "cow");
    free(pids);
    note("%d children verified %zu pages each", n, pages);
}

/* ------------------------------------------------------------------------ */
/* zero - fresh anonymous memory must be zero-filled                         */
/* ------------------------------------------------------------------------ */

/*
 * A page handed out still carrying its last owner's data is both a leak and a
 * corruption source, and it only shows after enough churn for the allocator to
 * hand back a dirty page. Dirty a batch, let those processes exit, then map
 * again in a fresh child and insist on zeroes.
 */
static void test_zero(int max_kids)
{
    const size_t len = 8 * PAGE_SIZE;
    int kids = max_kids < 8 ? max_kids : 8;
    int pids[8] = {0};
    int n = 0;

    for (int round = 0; round < 2; round++) {
        n = 0;
        for (int i = 0; i < kids; i++) {
            int pid = sys_fork();
            if (pid < 0) {
                break;
            }
            if (pid == 0) {
                unsigned char *p = map_anon(len);
                if (!p) {
                    sys_exit(KID_SETUP);
                }
                if (round > 0) {
                    for (size_t j = 0; j < len; j++) {
                        if (p[j] != 0) {
                            sys_exit(KID_ZERO_PAGE);
                        }
                    }
                }
                /* Leave it dirty for whoever gets these frames next. */
                memset(p, 0xA5 + i, len);
                sys_exit(KID_OK);
            }
            pids[n++] = pid;
        }
        reap_wave(pids, n, "zero");
    }

    CHECK(n > 0, "zero: could not fork any children");
    note("%d children checked %zu fresh bytes each", n, len);
}

/* ------------------------------------------------------------------------ */
/* fork - lifecycle accounting and slot reuse                                */
/* ------------------------------------------------------------------------ */

/*
 * Each child exits with a value derived from its index, so a status delivered
 * for the wrong child is visible rather than merely suspicious. Running three
 * waves and comparing high-water marks turns any per-process leak -- PCB,
 * page, descriptor -- into a falling number.
 */
static int fork_wave(int cap, int *hit_limit)
{
    int *pids = malloc(sizeof(int) * (size_t)cap);
    int n = 0;

    *hit_limit = 0;
    if (!pids) {
        CHECK(0, "fork: out of memory");
        return 0;
    }

    for (int i = 0; i < cap; i++) {
        int pid = sys_fork();
        if (pid < 0) {
            *hit_limit = 1;
            break;
        }
        if (pid == 0) {
            sys_exit(i % 97);
        }
        pids[n++] = pid;
    }

    for (int i = 0; i < n; i++) {
        int status = -1;
        int got = sys_waitpid(pids[i], &status, 0);

        g_checks++;
        if (got != pids[i]) {
            report_fail("fork: waitpid(%d) returned %d", pids[i], got);
            continue;
        }
        g_checks++;
        if (status != i % 97) {
            report_fail("fork: child %d exited %d, expected %d", pids[i], status, i % 97);
        }
    }

    free(pids);
    return n;
}

static void test_fork(int max_kids)
{
    int high[3];
    int limited[3];

    for (int w = 0; w < 3; w++) {
        high[w] = fork_wave(max_kids, &limited[w]);
        note("wave %d forked %d%s", w, high[w], limited[w] ? " (hit the process limit)" : "");
    }

    CHECK(high[0] > 0, "fork: first wave forked nothing");
    CHECK(high[1] >= high[0], "fork: wave 1 reached %d after wave 0 reached %d (leak)", high[1],
          high[0]);
    CHECK(high[2] >= high[0], "fork: wave 2 reached %d after wave 0 reached %d (leak)", high[2],
          high[0]);

    /* Nothing is left to reap once every wave has been collected. */
    CHECK(sys_waitpid(-1, NULL, 0) < 0, "fork: a child outlived its wave");
}

/* ------------------------------------------------------------------------ */
/* reap - orphans, reparenting and zombie cleanup                            */
/* ------------------------------------------------------------------------ */

/*
 * A child that exits before its parent waits, and a grandchild whose parent
 * dies first, are the two teardown orders that leak slots. The grandchild
 * checks its own reparenting; the wave-over-wave fork count checks that the
 * orphans were actually collected rather than parked as permanent zombies.
 */
static void test_reap(int max_kids)
{
    int kids = max_kids < 16 ? max_kids : 16;
    int pids[16];
    int n = 0;

    for (int i = 0; i < kids; i++) {
        int pid = sys_fork();
        if (pid < 0) {
            break;
        }
        if (pid == 0) {
            int grand = sys_fork();
            if (grand < 0) {
                sys_exit(KID_FORK);
            }
            if (grand == 0) {
                /* Outlive the parent, then confirm init adopted us. */
                sys_sleep(40);
                sys_exit(sys_getppid() == 1 ? KID_OK : KID_PPID);
            }
            sys_exit(KID_OK); /* orphan the grandchild deliberately */
        }
        pids[n++] = pid;
    }

    CHECK(n > 0, "reap: could not fork any children");
    reap_wave(pids, n, "reap");

    /* The grandchildren belong to init now; give them time to finish. */
    sys_sleep(200);

    int limited = 0;
    int after = fork_wave(kids, &limited);
    CHECK(after >= n, "reap: only %d slots available after orphaning %d children", after, n);
    note("%d orphans reparented, %d slots still available", n, after);
}

/* ------------------------------------------------------------------------ */
/* pipe - byte-exact transfer, blocking, and EOF                             */
/* ------------------------------------------------------------------------ */

/*
 * The payload is larger than any plausible pipe buffer, so the writer blocks
 * and the reader drains concurrently. Checking content and not just the total
 * catches bytes delivered out of order or a buffer wrapped one element short.
 */
static void test_pipe(void)
{
    const size_t total = 96 * 1024;
    const size_t chunk = 733; /* deliberately not a divisor of anything */
    uint32_t key = rnd();
    int fds[2];

    if (sys_pipe(fds) < 0) {
        CHECK(0, "pipe: creation failed");
        return;
    }

    int pid = sys_fork();
    if (pid < 0) {
        CHECK(0, "pipe: fork failed");
        sys_close(fds[0]);
        sys_close(fds[1]);
        return;
    }

    if (pid == 0) {
        unsigned char *buf = malloc(chunk);
        size_t got = 0;
        int verdict = KID_OK;

        sys_close(fds[1]);
        if (!buf) {
            sys_exit(KID_SETUP);
        }
        for (;;) {
            int n = sys_read(fds[0], buf, chunk);
            if (n <= 0) {
                break;
            }
            for (int i = 0; i < n; i++) {
                size_t off = got + (size_t)i;
                if (buf[i] != (unsigned char)(pattern_word(key, off / 4) >> ((off % 4) * 8))) {
                    verdict = KID_STREAM;
                }
            }
            got += (size_t)n;
        }
        sys_close(fds[0]);
        if (got != total) {
            verdict = KID_STREAM;
        }
        sys_exit(verdict);
    }

    sys_close(fds[0]);
    {
        unsigned char *buf = malloc(chunk);
        int short_write = 0;

        if (!buf) {
            CHECK(0, "pipe: out of memory");
        } else {
            for (size_t off = 0; off < total; off += chunk) {
                size_t want = total - off < chunk ? total - off : chunk;
                for (size_t i = 0; i < want; i++) {
                    size_t abs = off + i;
                    buf[i] = (unsigned char)(pattern_word(key, abs / 4) >> ((abs % 4) * 8));
                }
                if (write_full(fds[1], buf, want) != (int)want) {
                    short_write = 1;
                    break;
                }
            }
            free(buf);
        }
        CHECK(!short_write, "pipe: writer could not deliver the whole stream");
    }
    sys_close(fds[1]); /* the reader's EOF */

    int status = -1;
    CHECK(sys_waitpid(pid, &status, 0) == pid, "pipe: reader did not exit");
    CHECK(status == KID_OK, "pipe: reader reports %s", verdict_name(status));
    note("%zu bytes verified through a pipe", total);
}

/*
 * A single write larger than the pipe buffer has to make progress against a
 * reader that is already waiting. Nothing forces the writer to hand off what it
 * has buffered partway through, so it can fill the pipe, block for space, and
 * leave the reader asleep on bytes that already arrived -- both sides stuck.
 *
 * The reader is given time to reach its blocking read before the first byte
 * exists, which is what makes the deadlock reproducible rather than occasional.
 */
static int pipe_big_write(uint32_t key)
{
    const size_t len = 32768; /* several buffers' worth, in one write call */
    unsigned char *buf = malloc(len);
    int fds[2];

    if (!buf) {
        return KID_SETUP;
    }
    if (sys_pipe(fds) < 0) {
        free(buf);
        return KID_SETUP;
    }

    int pid = sys_fork();
    if (pid < 0) {
        sys_close(fds[0]);
        sys_close(fds[1]);
        free(buf);
        return KID_FORK;
    }
    if (pid == 0) {
        unsigned char *rb = malloc(len);
        sys_close(fds[1]);
        if (!rb) {
            sys_exit(KID_SETUP);
        }
        int n = read_full(fds[0], rb, len);
        sys_exit(n == (int)len && byte_pattern_check(rb, len, key) < 0 ? KID_OK : KID_STREAM);
    }

    sys_close(fds[0]);
    byte_pattern_fill(buf, len, key);
    sys_sleep(80); /* the reader is blocked on an empty pipe by now */

    int n = write_full(fds[1], buf, len);
    sys_close(fds[1]);
    free(buf);

    int status = -1;
    sys_waitpid(pid, &status, 0);
    return n == (int)len ? status : KID_STREAM;
}

static void test_pipe_big(void)
{
    uint32_t key = rnd();
    int pid = sys_fork();

    if (pid < 0) {
        CHECK(0, "pipe-big: fork failed");
        return;
    }
    if (pid == 0) {
        sys_exit(pipe_big_write(key));
    }

    /*
     * Run behind the deadline: this deadlocks outright when the hand-off is
     * missing, and a hang here would take the rest of the suite with it.
     */
    int status = -1;
    int got = wait_deadline(pid, &status, "pipe-big");

    g_checks++;
    if (got == pid) {
        if (status != KID_OK) {
            report_fail("pipe-big: reports %s (%d)", verdict_name(status), status);
        }
    }
    /* A deadline miss reported itself; killing the writer frees its reader. */
}

/*
 * Reading a pipe whose write end is closed must report EOF instead of blocking
 * forever, including when the closing happens after the reader is already
 * waiting. That second case is a lost-wakeup bug, and it hangs rather than
 * failing, so it runs last.
 */
static void test_pipe_eof(void)
{
    int fds[2];
    unsigned char byte = 0;

    if (sys_pipe(fds) < 0) {
        CHECK(0, "pipe-eof: creation failed");
        return;
    }

    /* Write end closed before any read: an immediate EOF. */
    sys_close(fds[1]);
    CHECK(sys_read(fds[0], &byte, 1) == 0, "pipe-eof: closed pipe did not report EOF");
    sys_close(fds[0]);

    if (sys_pipe(fds) < 0) {
        CHECK(0, "pipe-eof: second creation failed");
        return;
    }

    int pid = sys_fork();
    if (pid < 0) {
        CHECK(0, "pipe-eof: fork failed");
        sys_close(fds[0]);
        sys_close(fds[1]);
        return;
    }
    if (pid == 0) {
        /* Block in read first, so the parent's close has to wake us. */
        sys_close(fds[1]);
        unsigned char b = 0;
        int n = sys_read(fds[0], &b, 1);
        sys_exit(n == 0 ? KID_OK : KID_STREAM);
    }

    sys_close(fds[0]);
    sys_sleep(60); /* let the child reach its blocking read */
    sys_close(fds[1]);

    int status = -1;
    CHECK(sys_waitpid(pid, &status, 0) == pid, "pipe-eof: blocked reader never returned");
    CHECK(status == KID_OK, "pipe-eof: blocked reader reports %s", verdict_name(status));
}

/* ------------------------------------------------------------------------ */
/* signal - delivery, masking and coalescing                                 */
/* ------------------------------------------------------------------------ */

static volatile int g_sig_hits;
static volatile int g_sig_stop;
static volatile int g_sig_last;

static void count_handler(int sig)
{
    if (sig == SIGNAL_USR2) {
        g_sig_stop = 1;
        return;
    }
    g_sig_last = sig;
    g_sig_hits++;
}

/*
 * Pending signals coalesce, so the caught count is only bounded, not exact:
 * anything from one to the number sent is correct and zero is not. What must
 * be exact is the masking -- a blocked signal has to be pending and undelivered
 * until it is unblocked.
 */
static void test_signal(void)
{
    const int sends = 120;

    sys_sigrestore((uintptr_t)sys_sigreturn);
    sys_signal(SIGNAL_USR1, count_handler);
    sys_signal(SIGNAL_USR2, count_handler);

    /* Cleared before the fork, not after: the child inherits these counters. */
    g_sig_hits = 0;
    g_sig_stop = 0;

    int pid = sys_fork();
    if (pid < 0) {
        CHECK(0, "signal: fork failed");
        return;
    }
    if (pid == 0) {
        while (!g_sig_stop) {
            sys_yield();
        }
        sys_exit(g_sig_hits > 0 && g_sig_hits <= sends ? KID_OK : KID_SIGNAL);
    }

    for (int i = 0; i < sends; i++) {
        CHECK(sys_kill(pid, SIGNAL_USR1) == 0, "signal: kill %d rejected", i);
        sys_yield();
    }
    sys_kill(pid, SIGNAL_USR2);

    int status = -1;
    CHECK(sys_waitpid(pid, &status, 0) == pid, "signal: child did not exit");
    CHECK(status == KID_OK, "signal: child reports %s", verdict_name(status));

    /* Masking, checked on ourselves so the pending set is directly readable. */
    {
        sigset_t mask = 1u << (SIGNAL_USR1 - 1);
        sigset_t pending = 0;

        g_sig_hits = 0;
        g_sig_last = 0;

        CHECK(sys_sigprocmask(SIG_BLOCK, &mask, NULL) == 0, "signal: SIG_BLOCK failed");
        CHECK(sys_kill(sys_getpid(), SIGNAL_USR1) == 0, "signal: self-kill rejected");
        sys_yield();

        CHECK(g_sig_hits == 0, "signal: blocked SIGUSR1 was delivered anyway");
        CHECK(sys_sigpending(&pending) == 0, "signal: sigpending failed");
        CHECK((pending & mask) != 0, "signal: blocked SIGUSR1 is not pending");

        CHECK(sys_sigprocmask(SIG_UNBLOCK, &mask, NULL) == 0, "signal: SIG_UNBLOCK failed");
        sys_yield();
        CHECK(g_sig_hits == 1, "signal: unblocking delivered %d signals, expected 1", g_sig_hits);
        CHECK(g_sig_last == SIGNAL_USR1, "signal: delivered signal %d, expected %d", g_sig_last,
              SIGNAL_USR1);
    }

    sys_signal(SIGNAL_USR1, SIGNAL_DFL);
    sys_signal(SIGNAL_USR2, SIGNAL_DFL);
}

/*
 * A signal aimed at a process blocked in waitpid has to break the sleep. If it
 * does not, the handler never runs and this hangs -- which is the finding.
 */
static void test_signal_wake(void)
{
    sys_sigrestore((uintptr_t)sys_sigreturn);
    sys_signal(SIGNAL_USR1, count_handler);
    g_sig_hits = 0;

    int pid = sys_fork();
    if (pid < 0) {
        CHECK(0, "signal-wake: fork failed");
        return;
    }
    if (pid == 0) {
        /* Sleep long enough that a delivered signal is the only fast way out. */
        sys_sleep(400);
        sys_exit(g_sig_hits > 0 ? KID_OK : KID_SIGNAL);
    }

    sys_sleep(60);
    CHECK(sys_kill(pid, SIGNAL_USR1) == 0, "signal-wake: kill rejected");

    int status = -1;
    CHECK(sys_waitpid(pid, &status, 0) == pid, "signal-wake: sleeper never returned");
    CHECK(status == KID_OK, "signal-wake: sleeper reports %s", verdict_name(status));

    sys_signal(SIGNAL_USR1, SIGNAL_DFL);
}

/* ------------------------------------------------------------------------ */
/* fd - descriptor exhaustion, recovery and inheritance                      */
/* ------------------------------------------------------------------------ */

/*
 * This kernel allows a process only one descriptor per non-device file, so
 * exhaustion needs one file per descriptor. That restriction is checked here
 * too: it is a deliberate deviation from POSIX and worth pinning down.
 */
#define FD_PROBE_FILES 96

static void fd_probe_path(char *out, size_t len, int i)
{
    /* Names stay inside 8.3 or the filesystem shortens them into collisions. */
    snprintf(out, len, "/fd%d.tmp", i);
}

/*
 * Exhaustion has to fail cleanly and then recover completely. Two rounds with
 * the same high-water mark is the recovery evidence; a descriptor leaked on
 * the failing path makes the second round come up short.
 */
static void test_fd(void)
{
    int cap = FD_PROBE_FILES;
    int *fds = malloc(sizeof(int) * (size_t)cap);
    int high[2] = {0, 0};
    int made = 0;
    char path[32];

    if (!fds) {
        CHECK(0, "fd: out of memory");
        return;
    }

    for (int i = 0; i < cap; i++) {
        fd_probe_path(path, sizeof(path), i);
        int fd = sys_open(path, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC);
        if (fd < 0) {
            break;
        }
        sys_close(fd);
        made++;
    }
    if (made == 0) {
        CHECK(0, "fd: could not create any probe files");
        free(fds);
        return;
    }

    /* One descriptor per file, and a second on the same file is refused. */
    {
        fd_probe_path(path, sizeof(path), 0);
        int first = sys_open(path, VFS_O_RDONLY);
        CHECK(first >= 0, "fd: probe file would not open");
        if (first >= 0) {
            CHECK(sys_open(path, VFS_O_RDONLY) < 0,
                  "fd: the same file opened twice in one process");
            CHECK(sys_close(first) == 0, "fd: close of the probe failed");
        }
    }

    for (int round = 0; round < 2; round++) {
        int n = 0;
        while (n < made) {
            fd_probe_path(path, sizeof(path), n);
            int fd = sys_open(path, VFS_O_RDONLY);
            if (fd < 0) {
                break;
            }
            fds[n++] = fd;
        }
        high[round] = n;

        /* Every descriptor handed out must be distinct. */
        int dup_seen = 0;
        for (int i = 1; i < n && !dup_seen; i++) {
            for (int j = 0; j < i; j++) {
                if (fds[i] == fds[j]) {
                    dup_seen = 1;
                    break;
                }
            }
        }
        CHECK(!dup_seen, "fd: the same descriptor was handed out twice in round %d", round);

        for (int i = 0; i < n; i++) {
            CHECK(sys_close(fds[i]) == 0, "fd: close of %d failed", fds[i]);
        }
    }

    CHECK(high[0] > 1, "fd: only %d descriptor(s) could be held at once", high[0]);
    CHECK(high[1] >= high[0], "fd: round 1 reached %d after round 0 reached %d (leak)", high[1],
          high[0]);
    CHECK(sys_close(-1) < 0, "fd: close(-1) was accepted");
    CHECK(sys_close(cap * 8) < 0, "fd: close of an out-of-range descriptor was accepted");

    note("%d files, %d descriptors held, recovered to %d", made, high[0], high[1]);

    for (int i = 0; i < made; i++) {
        fd_probe_path(path, sizeof(path), i);
        CHECK(sys_unlink(path) == 0, "fd: unlink of %s failed", path);
    }
    free(fds);
}

/* ------------------------------------------------------------------------ */
/* file - content integrity through the filesystem                           */
/* ------------------------------------------------------------------------ */

#define FILE_PATH "/sfile.tmp"

/*
 * Sizes that straddle block boundaries are where short writes and off-by-one
 * block maps show up, so the round trip runs at several awkward lengths. The
 * create/unlink rounds afterwards look for inode or block leaks the same way
 * the fork test looks for slot leaks.
 */
static void test_file(void)
{
    static const size_t sizes[] = {1, 511, 512, 513, 4095, 4096, 4097, 12000};
    unsigned char *buf = malloc(16384);
    unsigned char *back = malloc(16384);

    if (!buf || !back) {
        CHECK(0, "file: out of memory");
        free(buf);
        free(back);
        return;
    }

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        size_t len = sizes[s];
        uint32_t key = rnd();

        int fd = sys_open(FILE_PATH, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC);
        if (fd < 0) {
            CHECK(0, "file: open for %zu bytes failed", len);
            continue;
        }

        byte_pattern_fill(buf, len, key);
        int wrote = write_full(fd, buf, len);
        CHECK(wrote == (int)len, "file: wrote %d of %zu bytes", wrote, len);

        CHECK(sys_lseek(fd, 0, VFS_SEEK_SET) == 0, "file: rewind failed");
        memset(back, 0, len);
        int read_back = read_full(fd, back, len);
        CHECK(read_back == (int)len, "file: read %d of %zu bytes", read_back, len);

        long bad = byte_pattern_check(back, len, key);
        CHECK(bad < 0, "file: content differs at byte %ld of %zu", bad, len);

        CHECK(sys_lseek(fd, 0, VFS_SEEK_END) == (off_t)len, "file: size is not %zu", len);

        /* A read past the end returns nothing rather than stale bytes. */
        CHECK(sys_read(fd, back, 16) == 0, "file: read past the end returned data");

        sys_close(fd);
    }

    /* Reopening must find the last content, not a cached earlier version. */
    {
        uint32_t key = rnd();
        size_t len = 8000;

        int fd = sys_open(FILE_PATH, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC);
        if (fd >= 0) {
            byte_pattern_fill(buf, len, key);
            write_full(fd, buf, len);
            sys_close(fd);

            fd = sys_open(FILE_PATH, VFS_O_RDONLY);
            if (fd >= 0) {
                memset(back, 0, len);
                int n = read_full(fd, back, len);
                CHECK(n == (int)len, "file: reopen read %d of %zu bytes", n, len);
                CHECK(byte_pattern_check(back, len, key) < 0,
                      "file: content changed across reopen");
                sys_close(fd);
            } else {
                CHECK(0, "file: reopen failed");
            }
        }
    }

    sys_unlink(FILE_PATH);
    CHECK(sys_open(FILE_PATH, VFS_O_RDONLY) < 0, "file: unlinked file still opens");

    /*
     * A name the filesystem shortens must still be findable under the name it
     * was created with. When create and lookup disagree about how to shorten
     * it, the file is created and immediately lost, and every retry appends
     * another directory entry for it.
     */
    {
        static const char *longname = "/stress_longname.tmp";
        int fd = sys_open(longname, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC);

        if (fd < 0) {
            CHECK(0, "file: a long name could not be created");
        } else {
            CHECK(write_full(fd, buf, 64) == 64, "file: long-name write failed");
            sys_close(fd);

            fd = sys_open(longname, VFS_O_RDONLY);
            CHECK(fd >= 0, "file: a created long name cannot be reopened");
            if (fd >= 0) {
                sys_close(fd);
            }
            CHECK(sys_unlink(longname) == 0, "file: long name could not be unlinked");
            CHECK(sys_open(longname, VFS_O_RDONLY) < 0,
                  "file: a second entry for the long name survived unlink");
        }
    }

    /* Create/unlink churn: a leak here shows up as a later round failing. */
    {
        int made[2] = {0, 0};

        for (int round = 0; round < 2; round++) {
            char path[64];
            for (int i = 0; i < 48; i++) {
                /* Names stay inside 8.3 so this measures leaks, not shortening. */
                snprintf(path, sizeof(path), "/sc%d.tmp", i);
                int fd = sys_open(path, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC);
                if (fd < 0) {
                    break;
                }
                write_full(fd, buf, 600);
                sys_close(fd);
                made[round]++;
            }
            for (int i = 0; i < made[round]; i++) {
                snprintf(path, sizeof(path), "/sc%d.tmp", i);
                CHECK(sys_unlink(path) == 0, "file: unlink of %s failed", path);
            }
        }
        CHECK(made[0] > 0, "file: could not create anything");
        CHECK(made[1] >= made[0], "file: round 1 created %d after round 0 created %d (leak)",
              made[1], made[0]);
        note("%d files created and removed per round", made[0]);
    }

    free(buf);
    free(back);
}

/* ------------------------------------------------------------------------ */
/* exec - argv delivery and address-space replacement                        */
/* ------------------------------------------------------------------------ */

/*
 * The child re-executes this same binary and checks a relation between two
 * argv strings that it can verify without knowing anything else, so a byte
 * lost anywhere in the vector is caught rather than just a missing pointer.
 */
static void test_exec(char **envp, int max_kids)
{
    int kids = max_kids < 8 ? max_kids : 8;
    int pids[8];
    int n = 0;

    for (int i = 0; i < kids; i++) {
        int pid = sys_fork();
        if (pid < 0) {
            break;
        }
        if (pid == 0) {
            char a[24], b[24];
            long v = 1000 + i * 37;

            snprintf(a, sizeof(a), "%ld", v);
            snprintf(b, sizeof(b), "%ld", v * 2 + 7);

            char *argv[] = {"stress", "--exec-child", a, b, NULL};
            sys_exec("/bin/stress.elf", argv, envp);
            sys_exec("stress.elf", argv, envp);
            sys_exit(KID_SETUP); /* exec must not return */
        }
        pids[n++] = pid;
    }

    CHECK(n > 0, "exec: could not fork any children");
    reap_wave(pids, n, "exec");
    note("%d children re-executed and verified their argv", n);
}

/* ------------------------------------------------------------------------ */
/* mix - subsystems running concurrently                                     */
/* ------------------------------------------------------------------------ */

/* Serial tests hold one lock at a time; the bugs that survive them need two. */
static int mix_memory(uint32_t key)
{
    const size_t len = 6 * PAGE_SIZE;
    unsigned char *p = map_anon(len);

    if (!p) {
        return KID_SETUP;
    }
    for (int round = 0; round < 8; round++) {
        pattern_fill(p, len, key + (uint32_t)round);
        sys_yield();
        if (pattern_check(p, len, key + (uint32_t)round) >= 0) {
            return KID_OWN_DATA;
        }
    }
    return KID_OK;
}

static int mix_fork(uint32_t key)
{
    for (int round = 0; round < 6; round++) {
        int pid = sys_fork();
        if (pid < 0) {
            /* The table being full is expected here, not a defect. */
            sys_sleep(10);
            continue;
        }
        if (pid == 0) {
            sys_exit((int)(key % 61));
        }
        int status = -1;
        if (sys_waitpid(pid, &status, 0) != pid || status != (int)(key % 61)) {
            return KID_FORK;
        }
    }
    return KID_OK;
}

static int mix_pipe(uint32_t key)
{
    const size_t len = 8192;
    unsigned char *buf = malloc(len);
    int fds[2];

    if (!buf) {
        return KID_SETUP;
    }
    if (sys_pipe(fds) < 0) {
        free(buf);
        return KID_SETUP;
    }

    int pid = sys_fork();
    if (pid < 0) {
        sys_close(fds[0]);
        sys_close(fds[1]);
        free(buf);
        return KID_FORK;
    }
    if (pid == 0) {
        unsigned char *rb = malloc(len);
        sys_close(fds[1]);
        if (!rb) {
            sys_exit(KID_SETUP);
        }
        int n = read_full(fds[0], rb, len);
        sys_exit(n == (int)len && byte_pattern_check(rb, len, key) < 0 ? KID_OK : KID_STREAM);
    }

    sys_close(fds[0]);
    byte_pattern_fill(buf, len, key);
    write_full(fds[1], buf, len);
    sys_close(fds[1]);
    free(buf);

    int status = -1;
    sys_waitpid(pid, &status, 0);
    return status == KID_OK ? KID_OK : KID_STREAM;
}

static int mix_file(uint32_t key)
{
    char path[64];
    unsigned char *buf = malloc(2048);
    unsigned char *back = malloc(2048);
    int verdict = KID_OK;

    if (!buf || !back) {
        free(buf);
        free(back);
        return KID_SETUP;
    }
    snprintf(path, sizeof(path), "/sm%u.tmp", key % 1000);

    for (int round = 0; round < 5 && verdict == KID_OK; round++) {
        uint32_t k = key + (uint32_t)round;
        int fd = sys_open(path, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC);

        if (fd < 0) {
            verdict = KID_SETUP;
            break;
        }
        byte_pattern_fill(buf, 2048, k);
        if (write_full(fd, buf, 2048) != 2048 || sys_lseek(fd, 0, VFS_SEEK_SET) != 0
            || read_full(fd, back, 2048) != 2048 || byte_pattern_check(back, 2048, k) >= 0) {
            verdict = KID_FILE;
        }
        sys_close(fd);
    }

    sys_unlink(path);
    free(buf);
    free(back);
    return verdict;
}

static int mix_signal(uint32_t key)
{
    (void)key;
    sys_sigrestore((uintptr_t)sys_sigreturn);
    sys_signal(SIGNAL_USR1, count_handler);
    g_sig_hits = 0;

    for (int i = 0; i < 40; i++) {
        if (sys_kill(sys_getpid(), SIGNAL_USR1) != 0) {
            return KID_SIGNAL;
        }
        sys_yield();
    }
    return g_sig_hits > 0 ? KID_OK : KID_SIGNAL;
}

static void test_mix(int max_kids)
{
    static int (*const workers[])(uint32_t) = {mix_memory, mix_fork, mix_pipe, mix_file,
                                               mix_signal};
    const int nworkers = (int)(sizeof(workers) / sizeof(workers[0]));
    int kids = max_kids < 20 ? max_kids : 20;
    int pids[20];
    int n = 0;

    for (int i = 0; i < kids; i++) {
        uint32_t key = rnd();
        int pid = sys_fork();

        if (pid < 0) {
            break;
        }
        if (pid == 0) {
            sys_exit(workers[i % nworkers](key));
        }
        pids[n++] = pid;
    }

    CHECK(n > 0, "mix: could not fork any workers");
    reap_wave(pids, n, "mix");
    note("%d workers across %d subsystems", n, nworkers);
}

/* ------------------------------------------------------------------------ */
/* stack - deep recursion against the user stack                             */
/* ------------------------------------------------------------------------ */

static int recurse(int depth, uint32_t key)
{
    volatile unsigned char frame[512];

    for (size_t i = 0; i < sizeof(frame); i++) {
        frame[i] = (unsigned char)(key + i + (uint32_t)depth);
    }
    if (depth > 0 && recurse(depth - 1, key) != 0) {
        return -1;
    }
    /* Checked after the recursion so a callee that scribbled on us is caught. */
    for (size_t i = 0; i < sizeof(frame); i++) {
        if (frame[i] != (unsigned char)(key + i + (uint32_t)depth)) {
            return -1;
        }
    }
    return 0;
}

/*
 * Runs in a child: overrunning the stack should kill that process and nothing
 * else, so the parent survives to report it either way.
 */
static void test_stack(void)
{
    uint32_t key = rnd();
    int pid = sys_fork();

    if (pid < 0) {
        CHECK(0, "stack: fork failed");
        return;
    }
    if (pid == 0) {
        sys_exit(recurse(120, key) == 0 ? KID_OK : KID_OWN_DATA);
    }

    int status = -1;
    CHECK(sys_waitpid(pid, &status, 0) == pid, "stack: child did not exit");
    CHECK(status == KID_OK, "stack: frames were corrupted across recursion (%s)",
          verdict_name(status));
}

/* ------------------------------------------------------------------------ */
/* Runner                                                                    */
/* ------------------------------------------------------------------------ */

struct stress_test {
    const char *name;
    const char *desc;
    void (*fn)(int max_kids, char **envp);
};

static void run_cow(int k, char **e)
{
    (void)e;
    test_cow(k);
}
static void run_zero(int k, char **e)
{
    (void)e;
    test_zero(k);
}
static void run_fork(int k, char **e)
{
    (void)e;
    test_fork(k);
}
static void run_reap(int k, char **e)
{
    (void)e;
    test_reap(k);
}
static void run_pipe(int k, char **e)
{
    (void)k;
    (void)e;
    test_pipe();
}
static void run_pipe_big(int k, char **e)
{
    (void)k;
    (void)e;
    test_pipe_big();
}
static void run_pipe_eof(int k, char **e)
{
    (void)k;
    (void)e;
    test_pipe_eof();
}
static void run_signal(int k, char **e)
{
    (void)k;
    (void)e;
    test_signal();
}
static void run_sigwake(int k, char **e)
{
    (void)k;
    (void)e;
    test_signal_wake();
}
static void run_fd(int k, char **e)
{
    (void)k;
    (void)e;
    test_fd();
}
static void run_file(int k, char **e)
{
    (void)k;
    (void)e;
    test_file();
}
static void run_exec(int k, char **e)
{
    test_exec(e, k);
}
static void run_mix(int k, char **e)
{
    (void)e;
    test_mix(k);
}
static void run_stack(int k, char **e)
{
    (void)k;
    (void)e;
    test_stack();
}

static const struct stress_test g_tests[] = {
    {"cow", "copy-on-write isolation under a fork storm", run_cow},
    {"zero", "fresh anonymous memory is zero-filled", run_zero},
    {"fork", "fork/exit accounting and slot reuse", run_fork},
    {"reap", "orphans, reparenting and zombie cleanup", run_reap},
    {"stack", "deep recursion against the user stack", run_stack},
    {"pipe", "byte-exact pipe transfer under blocking", run_pipe},
    {"pipe-big", "a write larger than the pipe buffer", run_pipe_big},
    {"pipe-eof", "pipe EOF, including a reader already blocked", run_pipe_eof},
    {"signal", "signal delivery, masking and coalescing", run_signal},
    {"sigwake", "a signal wakes a sleeping process", run_sigwake},
    {"fd", "descriptor exhaustion and recovery", run_fd},
    {"file", "filesystem content integrity and churn", run_file},
    {"exec", "argv delivery across exec", run_exec},
    {"mix", "several subsystems running concurrently", run_mix},
};

#define NTESTS ((int)(sizeof(g_tests) / sizeof(g_tests[0])))

static void usage(void)
{
    printf("usage: stress [-n iters] [-s seed] [-k maxkids] [-v] [-l] [test...]\n");
    printf("  -n  repeat the selected tests this many times (default 1)\n");
    printf("  -s  seed the PRNG; the same seed replays the same run\n");
    printf("  -k  cap on children per wave (default %d)\n", DEFAULT_MAX_KIDS);
    printf("  -v  print per-test detail\n");
    printf("  -l  list the tests and exit\n");
}

static void list_tests(void)
{
    printf("tests:\n");
    for (int i = 0; i < NTESTS; i++) {
        printf("  %-9s %s\n", g_tests[i].name, g_tests[i].desc);
    }
}

/*
 * Re-executed by test_exec. argv[2] and argv[3] must still satisfy the
 * relation the parent built into them, which fails if any byte of the vector
 * was lost or if the strings were truncated.
 */
static int exec_child_main(int argc, char **argv)
{
    if (argc != 4) {
        return KID_ARGV;
    }
    if (atol(argv[3]) != atol(argv[2]) * 2 + 7) {
        return KID_ARGV;
    }
    if (strcmp(argv[1], "--exec-child") != 0 || strcmp(argv[0], "stress") != 0) {
        return KID_ARGV;
    }
    return KID_OK;
}

int main(int argc, char **argv, char **envp)
{
    uint32_t seed = 0;
    int iters = 1;
    int max_kids = DEFAULT_MAX_KIDS;
    int selected[NTESTS];
    int nselected = 0;

    if (argc > 1 && strcmp(argv[1], "--exec-child") == 0) {
        sys_exit(exec_child_main(argc, argv));
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            list_tests();
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            iters = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            seed = (uint32_t)atol(argv[++i]);
        } else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            max_kids = atoi(argv[++i]);
        } else if (argv[i][0] == '-') {
            printf("stress: unknown option %s\n", argv[i]);
            usage();
            return 2;
        } else {
            int found = -1;
            for (int t = 0; t < NTESTS; t++) {
                if (strcmp(argv[i], g_tests[t].name) == 0) {
                    found = t;
                    break;
                }
            }
            if (found < 0) {
                printf("stress: no test named %s\n", argv[i]);
                list_tests();
                return 2;
            }
            if (nselected < NTESTS) {
                selected[nselected++] = found;
            }
        }
    }

    if (iters < 1) {
        iters = 1;
    }
    if (max_kids < 1) {
        max_kids = 1;
    }
    if (nselected == 0) {
        for (int t = 0; t < NTESTS; t++) {
            selected[nselected++] = t;
        }
    }
    if (seed == 0) {
        /* Nothing here is a clock, so mix in what does vary between runs. */
        seed = (uint32_t)sys_getpid() * 2654435761u + (uint32_t)(uintptr_t)&seed;
    }

    printf("[stress] seed %u, %d iteration(s), up to %d children per wave\n", seed, iters,
           max_kids);

    for (int it = 0; it < iters; it++) {
        if (iters > 1) {
            printf("[stress] iteration %d/%d\n", it + 1, iters);
        }
        for (int s = 0; s < nselected; s++) {
            const struct stress_test *t = &g_tests[selected[s]];
            int before_checks = g_checks;
            int before_failures = g_failures;

            /*
             * Announced before it runs, so a hang names the test that hung.
             * The verdict repeats the name because the kernel logs a line per
             * fork and per exit, and those land in between.
             */
            printf("  %-9s %s ...\n", t->name, t->desc);

            /* Each test starts from a seed derived only from the run seed. */
            g_rng = (uint64_t)seed * 0x2545F4914F6CDD1DULL + (uint64_t)(selected[s] + 1) * 977
                    + (uint64_t)it * 104729;
            if (g_rng == 0) {
                g_rng = 0x9E3779B97F4A7C15ULL;
            }

            t->fn(max_kids, envp);

            int ran = g_checks - before_checks;
            int failed = g_failures - before_failures;

            if (failed == 0) {
                printf("  %-9s ok (%d checks)\n", t->name, ran);
            } else {
                printf("  %-9s FAILED (%d of %d checks)\n", t->name, failed, ran);
            }
        }
    }

    printf("[stress] %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0) {
        printf("[stress] FAILED -- replay with: stress -s %u\n", seed);
        return 1;
    }
    printf("[stress] all clear\n");
    return 0;
}
