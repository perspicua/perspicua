/*
 * test_libc.c - User-space libc integrity test suite.
 *
 * Covers: string functions, malloc/free/realloc, printf/snprintf, ctype
 * classification/case-conversion, errno, and when stdout holds output back.
 * Each test group prints a PASS/FAIL summary line. Individual check failures
 * print the source location so regressions are easy to pinpoint.
 *
 * Exit status: 0 if all checks pass, 1 if any fail.
 */

#include <limits.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "assert.h"
#include "ctype.h"
#include "errno.h"
#include "setjmp.h"
#include "signal.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "syscall.h"
#include "term.h"
#include "wait.h"

// Minimal test framework

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (cond) {                                                           \
            g_passed++;                                                       \
        } else {                                                              \
            g_failed++;                                                       \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                     \
    } while (0)

// Run fn(), report PASS if no new failures occurred.
static void run_group(const char *name, void (*fn)(void))
{
    int before = g_failed;
    fn();
    if (g_failed == before) {
        printf("[PASS] %s\n", name);
    } else {
        printf("[FAIL] %s (%d check(s) failed)\n", name, g_failed - before);
    }
}

// assert() smoke test

static void test_assert(void)
{
    // assert on a true condition must not abort.
    assert(1 == 1);
    assert(0 == 0);
    assert((void *)0 == NULL);
    CHECK(1); // Reached only if the asserts above did not abort.

    // assert(0) would call __assert_fail and exit — not tested here.
}

// string.h

static void test_strlen(void)
{
    CHECK(strlen("") == 0);
    CHECK(strlen("a") == 1);
    CHECK(strlen("hello") == 5);
    CHECK(strlen("hello\0world") == 5); // Stops at first NUL.
}

static void test_strcpy(void)
{
    char buf[32];
    CHECK(strcpy(buf, "hello") == buf);
    CHECK(strcmp(buf, "hello") == 0);
    CHECK(buf[5] == '\0');
}

static void test_strncpy(void)
{
    char buf[8];
    memset(buf, 0xFF, sizeof(buf));
    strncpy(buf, "hi", 8);
    CHECK(buf[0] == 'h');
    CHECK(buf[1] == 'i');
    CHECK(buf[2] == '\0'); // Rest padded with NULs.
    CHECK(buf[7] == '\0');

    // Truncation: no NUL appended when src >= count.
    char trunc[3];
    strncpy(trunc, "abcde", 3);
    CHECK(trunc[0] == 'a' && trunc[1] == 'b' && trunc[2] == 'c');
}

static void test_strcat(void)
{
    char buf[32] = "foo";
    CHECK(strcat(buf, "bar") == buf);
    CHECK(strcmp(buf, "foobar") == 0);
}

static void test_strncat(void)
{
    char buf[32] = "foo";
    strncat(buf, "barbaz", 3);
    CHECK(strcmp(buf, "foobar") == 0);
    CHECK(buf[6] == '\0');
}

static void test_strcmp(void)
{
    CHECK(strcmp("abc", "abc") == 0);
    CHECK(strcmp("abc", "abd") < 0);
    CHECK(strcmp("abd", "abc") > 0);
    CHECK(strcmp("", "") == 0);
    CHECK(strcmp("a", "") > 0);
    CHECK(strcmp("", "a") < 0);
}

static void test_strncmp(void)
{
    CHECK(strncmp("abcX", "abcY", 3) == 0); // First 3 chars match.
    CHECK(strncmp("abcX", "abcY", 4) != 0);
    CHECK(strncmp("abc", "abc", 0) == 0); // Zero count always equal.
}

static void test_strchr(void)
{
    const char *s = "hello";
    CHECK(strchr(s, 'e') == s + 1);
    CHECK(strchr(s, 'z') == NULL);
    CHECK(strchr(s, '\0') == s + 5); // strchr finds the NUL terminator.
}

static void test_strrchr(void)
{
    const char *s = "hello";
    CHECK(strrchr(s, 'l') == s + 3); // Last 'l' is at index 3.
    CHECK(strrchr(s, 'z') == NULL);
}

static void test_strstr(void)
{
    CHECK(strstr("foobar", "bar") != NULL);
    CHECK(strstr("foobar", "baz") == NULL);
    CHECK(strstr("foobar", "") != NULL); // Empty needle returns haystack.
    CHECK(strstr("foobar", "foobar") != NULL);
    CHECK(strstr("foobar", "foobarbaz") == NULL);
}

static void test_strtok_r(void)
{
    char buf[] = "one,two,,three";
    char *save;
    char *tok;

    tok = strtok_r(buf, ",", &save);
    CHECK(tok != NULL && strcmp(tok, "one") == 0);

    tok = strtok_r(NULL, ",", &save);
    CHECK(tok != NULL && strcmp(tok, "two") == 0);

    tok = strtok_r(NULL, ",", &save); // Empty field skipped.
    CHECK(tok != NULL && strcmp(tok, "three") == 0);

    tok = strtok_r(NULL, ",", &save);
    CHECK(tok == NULL);
}

static void test_memset(void)
{
    char buf[16];
    memset(buf, 0xAB, sizeof(buf));
    for (int i = 0; i < 16; i++) {
        CHECK((unsigned char)buf[i] == 0xAB);
    }
    memset(buf, 0, sizeof(buf));
    for (int i = 0; i < 16; i++) {
        CHECK(buf[i] == 0);
    }
}

static void test_memcpy(void)
{
    const char src[] = "abcdefgh";
    char dst[9] = {0};
    memcpy(dst, src, sizeof(src));
    CHECK(memcmp(dst, src, sizeof(src)) == 0);

    // Unaligned sizes.
    char d3[3];
    memcpy(d3, "xyz", 3);
    CHECK(d3[0] == 'x' && d3[1] == 'y' && d3[2] == 'z');
}

static void test_strnlen(void)
{
    CHECK(strnlen("abc", 10) == 3); // Terminator found before the limit.
    CHECK(strnlen("abc", 3) == 3);
    CHECK(strnlen("abcdef", 3) == 3); // Limit reached first.
    CHECK(strnlen("abc", 0) == 0);
    CHECK(strnlen("", 5) == 0);
    CHECK(strnlen(NULL, 5) == 0);

    // No terminator within the limit: must stop at n, not scan past the end.
    char unterminated[4] = {'a', 'b', 'c', 'd'};
    CHECK(strnlen(unterminated, 4) == 4);
}

static void test_strdup(void)
{
    const char *src = "duplicate me";
    char *dup = strdup(src);
    CHECK(dup != NULL);
    CHECK(strcmp(dup, src) == 0);
    CHECK(dup != src); // A copy, not the same storage.
    dup[0] = 'D';
    CHECK(src[0] == 'd'); // Writing the copy must not touch the source.
    free(dup);

    char *empty = strdup("");
    CHECK(empty != NULL && strlen(empty) == 0);
    free(empty);

    CHECK(strdup(NULL) == NULL);
}

static void test_strndup(void)
{
    char *trunc = strndup("abcdef", 3);
    CHECK(trunc != NULL);
    CHECK(strcmp(trunc, "abc") == 0);
    free(trunc);

    char *whole = strndup("abc", 10); // n longer than the source.
    CHECK(whole != NULL && strcmp(whole, "abc") == 0);
    free(whole);

    char *none = strndup("abc", 0);
    CHECK(none != NULL && strlen(none) == 0);
    free(none);

    // Must terminate at n even when the source has no NUL in range.
    char unterminated[3] = {'x', 'y', 'z'};
    char *bounded = strndup(unterminated, 3);
    CHECK(bounded != NULL);
    CHECK(strlen(bounded) == 3);
    CHECK(memcmp(bounded, "xyz", 3) == 0);
    free(bounded);

    CHECK(strndup(NULL, 5) == NULL);
}

static void test_memmove(void)
{
    // Forward overlap: src < dst.
    char buf[16] = "abcdefgh";
    memmove(buf + 2, buf, 6); // "ab" -> "ababcdef" at [2..7].
    CHECK(buf[2] == 'a' && buf[7] == 'f');

    // Backward overlap: dst < src.
    char buf2[16] = "abcdefgh";
    memmove(buf2, buf2 + 2, 6);
    CHECK(buf2[0] == 'c' && buf2[5] == 'h');
}

// malloc / free / realloc

static void test_malloc_basic(void)
{
    void *p = malloc(64);
    CHECK(p != NULL);
    memset(p, 0xCC, 64);
    CHECK(*(unsigned char *)p == 0xCC);
    free(p);
}

static void test_malloc_zero(void)
{
    // malloc(0) may return NULL or a unique pointer; must not crash.
    void *p = malloc(0);
    free(p);  // Freeing NULL is always safe.
    CHECK(1); // Reached without crash.
}

static void test_malloc_multiple(void)
{
    // Allocate several blocks and verify they don't overlap.
    const int N = 8;
    void *ptrs[8];
    for (int i = 0; i < N; i++) {
        ptrs[i] = malloc(128);
        CHECK(ptrs[i] != NULL);
        memset(ptrs[i], (unsigned char)i, 128);
    }
    // Verify no block was overwritten by a subsequent allocation.
    for (int i = 0; i < N; i++) {
        unsigned char *p = ptrs[i];
        int ok = 1;
        for (int j = 0; j < 128; j++) {
            if (p[j] != (unsigned char)i) {
                ok = 0;
                break;
            }
        }
        CHECK(ok);
        free(ptrs[i]);
    }
}

static void test_malloc_coalesce(void)
{
    /* Free several adjacent blocks then allocate a larger one. The allocator
     * must coalesce freed blocks to satisfy the larger request from the same
     * chunk rather than always growing the heap. */
    void *a = malloc(256);
    void *b = malloc(256);
    void *c = malloc(256);
    CHECK(a != NULL && b != NULL && c != NULL);
    free(a);
    free(b);
    free(c);
    // Allocate something that fits in the coalesced region.
    void *big = malloc(512);
    CHECK(big != NULL);
    memset(big, 0, 512);
    free(big);
}

static void test_calloc(void)
{
    unsigned char *p = calloc(32, sizeof(unsigned char));
    CHECK(p != NULL);
    int zeroed = 1;
    for (int i = 0; i < 32; i++) {
        if (p[i] != 0) {
            zeroed = 0;
            break;
        }
    }
    CHECK(zeroed);
    free(p);
}

static void test_realloc(void)
{
    // Grow an allocation.
    char *p = malloc(32);
    CHECK(p != NULL);
    memset(p, 'A', 32);
    p = realloc(p, 128);
    CHECK(p != NULL);
    // Original bytes must be preserved.
    int ok = 1;
    for (int i = 0; i < 32; i++) {
        if (p[i] != 'A') {
            ok = 0;
            break;
        }
    }
    CHECK(ok);
    free(p);

    // realloc(NULL, size) must behave like malloc.
    char *q = realloc(NULL, 64);
    CHECK(q != NULL);
    free(q);

    // realloc(ptr, 0) must behave like free.
    char *r = malloc(16);
    CHECK(r != NULL);
    void *res = realloc(r, 0);
    CHECK(res == NULL); // Returns NULL after freeing.
}

// printf / snprintf

static void test_snprintf_basic(void)
{
    char buf[64];
    int n;

    n = snprintf(buf, sizeof(buf), "hello");
    CHECK(n == 5 && strcmp(buf, "hello") == 0);

    n = snprintf(buf, sizeof(buf), "%d", 42);
    CHECK(n == 2 && strcmp(buf, "42") == 0);

    n = snprintf(buf, sizeof(buf), "%d", -7);
    CHECK(n == 2 && strcmp(buf, "-7") == 0);

    n = snprintf(buf, sizeof(buf), "%u", 255u);
    CHECK(n == 3 && strcmp(buf, "255") == 0);

    n = snprintf(buf, sizeof(buf), "%x", 0xdeadu);
    CHECK(strcmp(buf, "dead") == 0);

    n = snprintf(buf, sizeof(buf), "%s %s", "foo", "bar");
    CHECK(n == 7 && strcmp(buf, "foo bar") == 0);

    n = snprintf(buf, sizeof(buf), "%c", 'Z');
    CHECK(n == 1 && buf[0] == 'Z');
}

static void test_snprintf_bounds(void)
{
    char buf[5];

    // Output must be truncated and always NUL-terminated.
    int n = snprintf(buf, sizeof(buf), "hello world");
    CHECK(buf[4] == '\0'); // Always NUL-terminated.
    CHECK(strncmp(buf, "hell", 4) == 0);
    CHECK(n == 11); // Returns the would-be length.

    // Minimal buffer: size=1 means only the NUL fits.
    char tiny[1];
    snprintf(tiny, 1, "abc");
    CHECK(tiny[0] == '\0');
}

static void test_snprintf_percent(void)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "100%%");
    CHECK(strcmp(buf, "100%") == 0);
}

// The C99 sizing idiom: with nowhere to write, snprintf still reports the full length.
static void test_snprintf_sizing(void)
{
    CHECK(snprintf(NULL, 0, "hello %d", 12345) == 11);
    char z[4] = "xyz";
    CHECK(snprintf(z, 0, "hello") == 5);
    CHECK(strcmp(z, "xyz") == 0);
}

static int formats(const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "  got \"%s\", want \"%s\"\n", got, want);
        return 0;
    }
    return 1;
}

static void test_snprintf_precision(void)
{
    char b[32];
    snprintf(b, sizeof(b), "%.3d", 5);
    CHECK(formats(b, "005"));
    snprintf(b, sizeof(b), "[%.0d]", 0);
    CHECK(formats(b, "[]"));
    snprintf(b, sizeof(b), "%5.3d", 7);
    CHECK(formats(b, "  007"));
    snprintf(b, sizeof(b), "%-6.2x|", 15);
    CHECK(formats(b, "0f    |"));
    // GCC rejects this pairing in a literal, so it goes in through a runtime format.
    char zero_and_precision[] = "%08.3d";
    snprintf(b, sizeof(b), zero_and_precision, 5);
    CHECK(formats(b, "     005"));
    snprintf(b, sizeof(b), "%+.2d", 3);
    CHECK(formats(b, "+03"));
    snprintf(b, sizeof(b), "%.4d", -42);
    CHECK(formats(b, "-0042"));
    snprintf(b, sizeof(b), "%-5d|%05d", -3, -3);
    CHECK(formats(b, "-3   |-0003"));
}

static void test_snprintf_hash(void)
{
    char b[32];
    snprintf(b, sizeof(b), "%#x %#X", 255, 255);
    CHECK(formats(b, "0xff 0XFF"));
    snprintf(b, sizeof(b), "%#o %#o", 8, 0);
    CHECK(formats(b, "010 0"));
    snprintf(b, sizeof(b), "%#x", 0);
    CHECK(formats(b, "0"));
    snprintf(b, sizeof(b), "%#08x", 255);
    CHECK(formats(b, "0x0000ff"));
}

static void test_snprintf_lengths(void)
{
    char b[48];
    snprintf(b, sizeof(b), "%hhd %hd %hhu %hu", 300, 70000, 257, 65537);
    CHECK(formats(b, "44 4464 1 1"));
    snprintf(b, sizeof(b), "%jd %td %zu", (intmax_t)-5, (ptrdiff_t)7, (size_t)123);
    CHECK(formats(b, "-5 7 123"));
    snprintf(b, sizeof(b), "%d %lld", -2147483647 - 1, -9223372036854775807LL - 1);
    CHECK(formats(b, "-2147483648 -9223372036854775808"));
    snprintf(b, sizeof(b), "%lu %lx", 18446744073709551615UL, 0xdeadbeefcafeUL);
    CHECK(formats(b, "18446744073709551615 deadbeefcafe"));
}

// fread returns short only at end of file or on an error, so it waits for a slow pipe.
static void test_fread_waits_for_pipe(void)
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    int reader = fork();
    if (reader == 0) {
        dup2(fds[0], 0);
        close(fds[0]);
        close(fds[1]);
        char buf[8] = {0};
        size_t n = fread(buf, 1, 6, stdin);
        _exit(n == 6 && memcmp(buf, "abcdef", 6) == 0 && !feof(stdin) ? 0 : 1);
    }
    close(fds[0]);
    // A reader that gave up early would otherwise kill this suite with SIGPIPE.
    sighandler_t old_pipe = signal(SIGPIPE, SIG_IGN);
    write(fds[1], "abc", 3);
    usleep(200 * 1000);
    write(fds[1], "def", 3);
    close(fds[1]);
    signal(SIGPIPE, old_pipe);
    int status = -1;
    waitpid(reader, &status, 0);
    CHECK(status == 0);
}

// ctype.h

static void test_isalnum(void)
{
    CHECK(isalnum('a') && isalnum('z') && isalnum('A') && isalnum('Z'));
    CHECK(isalnum('0') && isalnum('9'));
    CHECK(!isalnum(' ') && !isalnum('!') && !isalnum('\n') && !isalnum('_'));
}

static void test_isalpha(void)
{
    CHECK(isalpha('a') && isalpha('Z'));
    CHECK(!isalpha('0') && !isalpha('9')); // Digits are not alpha.
    CHECK(!isalpha(' ') && !isalpha('.'));
}

static void test_isupper(void)
{
    CHECK(isupper('A') && isupper('M') && isupper('Z'));
    CHECK(!isupper('a') && !isupper('z')); // Lowercase is not upper.
    CHECK(!isupper('5') && !isupper(' ') && !isupper('!'));
}

static void test_islower(void)
{
    CHECK(islower('a') && islower('m') && islower('z'));
    CHECK(!islower('A') && !islower('Z')); // Uppercase is not lower.
    CHECK(!islower('5') && !islower(' ') && !islower('!'));
}

static void test_isdigit(void)
{
    for (char c = '0'; c <= '9'; c++) {
        CHECK(isdigit(c));
    }
    CHECK(!isdigit('a') && !isdigit(' ') && !isdigit('/') && !isdigit(':'));
    // '/' and ':' are the bytes immediately outside the '0'-'9' range.
}

static void test_isspace(void)
{
    CHECK(isspace(' ') && isspace('\t') && isspace('\n'));
    CHECK(isspace('\v') && isspace('\f') && isspace('\r'));
    CHECK(!isspace('a') && !isspace('0'));
}

static void test_isblank(void)
{
    // isblank is narrower than isspace: only space and tab.
    CHECK(isblank(' ') && isblank('\t'));
    CHECK(!isblank('\n') && !isblank('\v') && !isblank('\f') && !isblank('\r'));
}

static void test_ispunct(void)
{
    CHECK(ispunct('!') && ispunct('.') && ispunct('@') && ispunct('~'));
    CHECK(ispunct('[') && ispunct('_') && ispunct('`'));
    CHECK(!ispunct('a') && !ispunct('5') && !ispunct(' '));
}

static void test_iscntrl(void)
{
    CHECK(iscntrl('\0') && iscntrl('\n') && iscntrl(0x1F) && iscntrl(0x7F)); // DEL
    CHECK(!iscntrl('a') && !iscntrl(' ') && !iscntrl('~'));
}

static void test_isxdigit(void)
{
    CHECK(isxdigit('0') && isxdigit('9'));
    CHECK(isxdigit('a') && isxdigit('f') && isxdigit('A') && isxdigit('F'));
    CHECK(!isxdigit('g') && !isxdigit('G') && !isxdigit('z'));
}

static void test_isgraph(void)
{
    // isgraph: any printable character EXCEPT space.
    CHECK(isgraph('a') && isgraph('Z') && isgraph('5'));
    CHECK(isgraph('!') && isgraph('~')); // Punctuation counts.
    CHECK(!isgraph(' '));                // Space is excluded.
    CHECK(!isgraph('\t') && !isgraph('\n') && !isgraph(0x1F) && !isgraph(0x7F));
}

static void test_isprint(void)
{
    // isprint: any printable character INCLUDING space.
    CHECK(isprint('a') && isprint('Z') && isprint('5'));
    CHECK(isprint('!') && isprint('~'));
    CHECK(isprint(' ')); // The one difference from isgraph.
    CHECK(!isprint('\t') && !isprint('\n') && !isprint(0x1F) && !isprint(0x7F));
}

static void test_toupper(void)
{
    CHECK(toupper('a') == 'A');
    CHECK(toupper('z') == 'Z');
    CHECK(toupper('A') == 'A'); // Already upper: unchanged.
    CHECK(toupper('5') == '5'); // Non-letter: unchanged.
    CHECK(toupper('!') == '!');
}

static void test_tolower(void)
{
    CHECK(tolower('A') == 'a');
    CHECK(tolower('Z') == 'z');
    CHECK(tolower('a') == 'a'); // Already lower: unchanged.
    CHECK(tolower('5') == '5'); // Non-letter: unchanged.
    CHECK(tolower('!') == '!');
}

static void test_ctype_bounds(void)
{
    // EOF and out-of-range values must be handled without indexing OOB.
    CHECK(!isalnum(EOF));
    CHECK(!isalpha(EOF));
    CHECK(!isdigit(EOF));
    CHECK(!isspace(EOF));
    CHECK(!isupper(EOF));
    CHECK(!islower(EOF));
    CHECK(!isgraph(EOF));
    CHECK(!isprint(EOF));

    CHECK(!isalnum(128));
    CHECK(!isalnum(255));
    CHECK(!isalnum(1000));
    CHECK(!isalnum(-100));
    CHECK(!isupper(200) && !islower(200));
    CHECK(!isgraph(200) && !isprint(200));

    /* toupper/tolower must return the argument unchanged, not crash or
     * table-index out of bounds. */
    CHECK(toupper(EOF) == EOF);
    CHECK(tolower(EOF) == EOF);
    CHECK(toupper(200) == 200);
    CHECK(tolower(200) == 200);
}

// setjmp / longjmp

static jmp_buf g_basic_jb;
static jmp_buf g_zero_jb;
static jmp_buf g_nest_jb;

static void test_setjmp_direct(void)
{
    jmp_buf local;
    // A setjmp with no matching longjmp reports the first-time return.
    CHECK(setjmp(local) == 0);
}

static void test_longjmp_value(void)
{
    volatile int before_jump = 0;
    int rc = setjmp(g_basic_jb);

    if (rc == 0) {
        before_jump = 1;
        longjmp(g_basic_jb, 42);
        // Unreached: falling through here would leave rc at 0 below.
    }

    CHECK(rc == 42);         // setjmp returned longjmp's value
    CHECK(before_jump == 1); // volatile local survived the jump
}

static void test_longjmp_zero(void)
{
    /* longjmp(buf, 0) must surface as 1 so it is never mistaken for the
     * first-time return. The guard stops a runaway loop if that rule breaks. */
    static volatile int attempts = 0;

    int rc = setjmp(g_zero_jb);
    if (rc == 0) {
        attempts++;
        if (attempts > 2) {
            CHECK(0); // longjmp(buf, 0) kept returning 0
            return;
        }
        longjmp(g_zero_jb, 0);
    }

    CHECK(rc == 1);
}

// Jumps out of three nested frames at once.
static void nest_level3(void)
{
    longjmp(g_nest_jb, 7);
}

static void nest_level2(void)
{
    nest_level3();
}

static void nest_level1(void)
{
    nest_level2();
}

// Recurses so a wrong sp after longjmp shows up as a wrong sum or a fault.
static int sum_to(int n)
{
    return n <= 0 ? 0 : n + sum_to(n - 1);
}

static void test_longjmp_nested(void)
{
    int rc = setjmp(g_nest_jb);
    if (rc == 0) {
        nest_level1();
    }

    CHECK(rc == 7);
    // The abandoned frames must leave a usable stack behind.
    CHECK(sum_to(10) == 55);
}

static void test_setjmp_independent(void)
{
    // Two buffers must not alias: jumping through one leaves the other set.
    jmp_buf a, b;
    volatile int hops = 0;

    if (setjmp(a) == 0) {
        if (setjmp(b) == 0) {
            hops = 1;
            longjmp(b, 3);
        }
        CHECK(hops == 1);
        hops = 2;
        longjmp(a, 5);
    }

    CHECK(hops == 2);
}

// strerror

static void test_strerror_keying(void)
{
    /* Guards the numbering collision: the internal EIO is 9 and
     * so is EBADF, so a table keyed on the internal codes would return the
     * I/O-error text for value 9. These must stay distinct and correct. */
    CHECK(EBADF == 9);
    CHECK(strcmp(strerror(9), "Bad file descriptor") == 0);
    CHECK(strcmp(strerror(EIO), "I/O error") == 0);
    CHECK(strcmp(strerror(EIO), strerror(EBADF)) != 0);

    /* ENOENT is 1 while ENOENT is 2 — an off-by-one keying would
     * report "Operation not permitted" for a missing file. */
    CHECK(strcmp(strerror(1), "Operation not permitted") == 0);
    CHECK(strcmp(strerror(2), "No such file or directory") == 0);
}

static void test_strerror_end_to_end(void)
{
    /* The real path: a failed syscall sets errno, and strerror must describe
     * it. This is what proves the table is keyed on the same numbering that
     * __pers_to_errno() produces. */
    errno = 0;
    int fd = open("/no/such/file", O_RDONLY);
    CHECK(fd == -1);
    CHECK(strcmp(strerror(errno), "No such file or directory") == 0);

    errno = 0;
    int ret = close(9999);
    CHECK(ret == -1);
    CHECK(strcmp(strerror(errno), "Bad file descriptor") == 0);
}

// errno

static void test_errno_open(void)
{
    // Opening a non-existent file must set errno to ENOENT.
    errno = 0;
    int fd = open("/no/such/file", O_RDONLY);
    CHECK(fd == -1);
    CHECK(errno == ENOENT);
}

static void test_errno_close(void)
{
    // Closing an invalid descriptor must set errno to EBADF.
    errno = 0;
    int ret = close(9999);
    CHECK(ret == -1);
    CHECK(errno == EBADF);
}

static void test_errno_preserved(void)
{
    // A successful call must not disturb errno.
    errno = EINVAL;
    int pid = getpid();
    CHECK(pid > 0);
    CHECK(errno == EINVAL); // Unchanged by the successful call.
}

// strerror

static void test_strerror_known(void)
{
    CHECK(strcmp(strerror(EPERM), "Operation not permitted") == 0);
    CHECK(strcmp(strerror(ENOENT), "No such file or directory") == 0);
    CHECK(strcmp(strerror(EIO), "I/O error") == 0);
    CHECK(strcmp(strerror(EBADF), "Bad file descriptor") == 0);
    CHECK(strcmp(strerror(ENOMEM), "Out of memory") == 0);
    CHECK(strcmp(strerror(EACCES), "Permission denied") == 0);
    CHECK(strcmp(strerror(EEXIST), "File exists") == 0);
    CHECK(strcmp(strerror(EINVAL), "Invalid argument") == 0);
    CHECK(strcmp(strerror(ENOSPC), "No space left on device") == 0);
    CHECK(strcmp(strerror(EPIPE), "Broken pipe") == 0);
    CHECK(strcmp(strerror(ENAMETOOLONG), "File name too long") == 0);
    CHECK(strcmp(strerror(ENOSYS), "Function not implemented") == 0);
    CHECK(strcmp(strerror(ELOOP), "Too many levels of symbolic links") == 0);
    CHECK(strcmp(strerror(ETIMEDOUT), "Connection timed out") == 0);
    CHECK(strcmp(strerror(ECONNREFUSED), "Connection refused") == 0);
}

static void test_strerror_aliases(void)
{
    /* EWOULDBLOCK and EOPNOTSUPP are #defined to EAGAIN/ENOTSUP, so they
     * must resolve to the exact same message rather than a separate,
     * possibly-diverging table entry. */
    CHECK(strcmp(strerror(EAGAIN), strerror(EWOULDBLOCK)) == 0);
    CHECK(strcmp(strerror(ENOTSUP), strerror(EOPNOTSUPP)) == 0);
    CHECK(strcmp(strerror(EWOULDBLOCK), "Resource temporarily unavailable") == 0);
    CHECK(strcmp(strerror(EOPNOTSUPP), "Operation not supported") == 0);
}

static void test_strerror_gaps(void)
{
    /* Numbers with no #define on this platform (holes in the errno
     * numbering) must fall back to "Unknown error <n>", not crash or
     * return a stale/garbage string. */
    char expected[32];
    int gaps[] = {15, 26, 31, 33, 37};

    for (size_t i = 0; i < sizeof(gaps) / sizeof(gaps[0]); i++) {
        snprintf(expected, sizeof(expected), "Unknown error %d", gaps[i]);
        CHECK(strcmp(strerror(gaps[i]), expected) == 0);
    }
}

static void test_strerror_out_of_range(void)
{
    char expected[32];

    // Negative values.
    snprintf(expected, sizeof(expected), "Unknown error %d", -1);
    CHECK(strcmp(strerror(-1), expected) == 0);

    // Just past the highest defined code (ECONNREFUSED == 111).
    snprintf(expected, sizeof(expected), "Unknown error %d", 112);
    CHECK(strcmp(strerror(112), expected) == 0);

    // Far out of range, well beyond any lookup table bound.
    snprintf(expected, sizeof(expected), "Unknown error %d", 999999);
    CHECK(strcmp(strerror(999999), expected) == 0);

    /* INT_MIN produces the longest possible text (25 chars). The comparison
     * buffer is deliberately wider than strerror's own, so a too-small buffer
     * there fails here instead of matching an equally-truncated expectation. */
    const int int_min = -2147483647 - 1;
    char wide[64];
    snprintf(wide, sizeof(wide), "Unknown error %d", int_min);
    CHECK(strcmp(strerror(int_min), wide) == 0);
    CHECK(strlen(strerror(int_min)) == 25);
}

static void test_strerror_nonnull(void)
{
    /* strerror() must never return NULL, even for nonsense input, since
     * callers commonly pass the result straight to printf("%s", ...). */
    CHECK(strerror(EINVAL) != NULL);
    CHECK(strerror(-12345) != NULL);
    CHECK(strerror(0) != NULL);
}

// qsort

static int int_cmp(const void *a, const void *b)
{
    int x = *(const int *)a;
    int y = *(const int *)b;
    return x < y ? -1 : x > y;
}

static int sorted(const int *v, int n)
{
    for (int i = 1; i < n; i++) {
        if (v[i - 1] > v[i]) {
            return 0;
        }
    }
    return 1;
}

static void test_qsort(void)
{
    int mixed[] = {5, -3, 9, 0, 9, 2, -3, 7, 1, 4};
    qsort(mixed, 10, sizeof(int), int_cmp);
    CHECK(sorted(mixed, 10));
    CHECK(mixed[0] == -3 && mixed[1] == -3 && mixed[9] == 9);

    int down[64];
    for (int i = 0; i < 64; i++) {
        down[i] = 64 - i;
    }
    qsort(down, 64, sizeof(int), int_cmp);
    CHECK(sorted(down, 64) && down[0] == 1 && down[63] == 64);

    int one[] = {42};
    qsort(one, 1, sizeof(int), int_cmp);
    CHECK(one[0] == 42);
    qsort(NULL, 0, sizeof(int), int_cmp);

    // Elements wider than a word move whole.
    struct wide {
        int key;
        char tag[12];
    } w[] = {{3, "three"}, {1, "one"}, {2, "two"}};
    qsort(w, 3, sizeof(w[0]), int_cmp);
    CHECK(strcmp(w[0].tag, "one") == 0 && strcmp(w[1].tag, "two") == 0
          && strcmp(w[2].tag, "three") == 0);
}

// rand / srand

static void test_rand(void)
{
    int first[8];
    srand(1);
    for (int i = 0; i < 8; i++) {
        first[i] = rand();
        CHECK(first[i] >= 0 && first[i] <= RAND_MAX);
    }

    // The same seed replays the same sequence.
    srand(1);
    for (int i = 0; i < 8; i++) {
        CHECK(rand() == first[i]);
    }

    srand(2);
    CHECK(rand() != first[0]);

    // Not stuck: the low bit is set about half the time.
    int odd = 0;
    for (int i = 0; i < 1000; i++) {
        odd += rand() & 1;
    }
    CHECK(odd > 400 && odd < 600);
}

// atexit

static void mark_a(void)
{
    write(1, "a", 1);
}

static void mark_b(void)
{
    write(1, "b", 1);
}

// Runs child() with stdout on a pipe and returns what it printed.
static int capture(void (*child)(void), char *out, int size)
{
    int fds[2];
    if (pipe(fds) != 0) {
        return -1;
    }
    int pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        child();
        _exit(127);
    }
    close(fds[1]);
    int n = 0;
    int r;
    while (n < size - 1 && (r = read(fds[0], out + n, (size_t)(size - 1 - n))) > 0) {
        n += r;
    }
    out[n] = '\0';
    close(fds[0]);
    int status;
    waitpid(pid, &status, 0);
    return n;
}

static void atexit_then_exit(void)
{
    atexit(mark_a);
    atexit(mark_b);
    exit(0);
}

// Returning from main must run the handlers too, so this goes through a fresh exec.
static void atexit_then_return(void)
{
    char *argv[] = {"/bin/test_libc.elf", "--atexit-child", NULL};
    execve(argv[0], argv, environ);
}

static void test_atexit(void)
{
    char out[16];
    CHECK(capture(atexit_then_exit, out, sizeof(out)) == 2);
    CHECK(strcmp(out, "ba") == 0);

    CHECK(capture(atexit_then_return, out, sizeof(out)) == 2);
    CHECK(strcmp(out, "ba") == 0);
}

// stdout buffering

// Run as a fresh process with stdout on a pipe, which is how a program in a pipeline starts.
static void on_usr1(int sig)
{
    (void)sig;
}

static int stdout_child(const char *name)
{
    if (strcmp(name, "held") == 0) {
        printf("x");
        write(1, "y", 1);
    } else if (strcmp(name, "fflush") == 0) {
        printf("x");
        fflush(stdout);
        write(1, "y", 1);
    } else if (strcmp(name, "_exit") == 0) {
        printf("x");
        _exit(0);
    } else if (strcmp(name, "fork") == 0) {
        printf("x");
        int pid = fork();
        if (pid == 0) {
            exit(0);
        }
        int status;
        waitpid(pid, &status, 0);
    } else if (strcmp(name, "exec") == 0) {
        printf("x");
        char *argv[] = {"/bin/seq.elf", "1", NULL};
        execve(argv[0], argv, environ);
    } else if (strcmp(name, "long") == 0) {
        static char line[601];
        memset(line, 'z', 600);
        fprintf(stdout, "%s", line);
    } else if (strcmp(name, "fclose") == 0) {
        printf("x");
        fclose(stdout);
        _exit(0);
    } else if (strcmp(name, "eintr") == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_usr1;
        sigaction(SIGUSR1, &sa, NULL);
        static char block[1024];
        memset(block, 'x', sizeof(block));
        for (int i = 0; i < 16; i++) {
            fwrite(block, 1, sizeof(block), stdout);
        }
        _exit(fflush(stdout) == 0 && !ferror(stdout) ? 0 : 1);
    }
    return 0;
}

static const char *stdout_case;

static void exec_stdout_case(void)
{
    char *argv[] = {"/bin/test_libc.elf", "--stdout-child", (char *)stdout_case, NULL};
    execve(argv[0], argv, environ);
}

static int prints(const char *name, char *out, int size)
{
    stdout_case = name;
    return capture(exec_stdout_case, out, size);
}

static void test_stdout_buffering(void)
{
    char out[1024];
    CHECK(prints("held", out, sizeof(out)) == 2 && strcmp(out, "yx") == 0);
    CHECK(prints("fflush", out, sizeof(out)) == 2 && strcmp(out, "xy") == 0);
    CHECK(prints("_exit", out, sizeof(out)) == 0);
    CHECK(prints("fork", out, sizeof(out)) == 1 && strcmp(out, "x") == 0);
    CHECK(prints("exec", out, sizeof(out)) == 3 && strcmp(out, "x1\n") == 0);

    int n = prints("long", out, sizeof(out));
    int all_z = n == 600;
    for (int i = 0; all_z && i < n; i++) {
        all_z = out[i] == 'z';
    }
    CHECK(all_z);
}

static void test_fclose_flushes_stdout(void)
{
    char out[16];
    CHECK(prints("fclose", out, sizeof(out)) == 1 && strcmp(out, "x") == 0);
}

/*
 * A signal that interrupts a flush to a full pipe must not cost any output: POSIX
 * sigaction defaults to no SA_RESTART, so the blocked write comes back with EINTR.
 */
static void test_stdout_survives_eintr(void)
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    stdout_case = "eintr";
    int writer = fork();
    if (writer == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        exec_stdout_case();
        _exit(127);
    }
    close(fds[1]);

    usleep(300 * 1000);
    kill(writer, SIGUSR1);
    usleep(300 * 1000);

    static char sink[4096];
    int total = 0, n;
    while ((n = read(fds[0], sink, sizeof(sink))) > 0) {
        total += n;
    }
    close(fds[0]);
    int status = -1;
    waitpid(writer, &status, 0);
    CHECK(total == 16 * 1024);
    CHECK(status == 0);
}

// term_decode

static int decode(const char *s, int *used)
{
    return term_decode((const unsigned char *)s, (int)strlen(s), used);
}

static void test_term_decode(void)
{
    int u;

    CHECK(decode("a", &u) == 'a' && u == 1);
    CHECK(decode("\r", &u) == TERM_KEY_ENTER && u == 1);
    CHECK(decode("\n", &u) == TERM_KEY_ENTER && u == 1);
    CHECK(decode("\x7f", &u) == TERM_KEY_BACKSPACE && u == 1);
    CHECK(decode("\b", &u) == TERM_KEY_BACKSPACE && u == 1);
    CHECK(decode("\xc3\xa9", &u) == 0xc3 && u == 1);

    CHECK(decode("\033[A", &u) == TERM_KEY_UP && u == 3);
    CHECK(decode("\033[B", &u) == TERM_KEY_DOWN && u == 3);
    CHECK(decode("\033[C", &u) == TERM_KEY_RIGHT && u == 3);
    CHECK(decode("\033[D", &u) == TERM_KEY_LEFT && u == 3);
    CHECK(decode("\033OA", &u) == TERM_KEY_UP && u == 3);
    CHECK(decode("\033OH", &u) == TERM_KEY_HOME && u == 3);
    CHECK(decode("\033[H", &u) == TERM_KEY_HOME && u == 3);
    CHECK(decode("\033[F", &u) == TERM_KEY_END && u == 3);
    CHECK(decode("\033[1~", &u) == TERM_KEY_HOME && u == 4);
    CHECK(decode("\033[7~", &u) == TERM_KEY_HOME && u == 4);
    CHECK(decode("\033[4~", &u) == TERM_KEY_END && u == 4);
    CHECK(decode("\033[8~", &u) == TERM_KEY_END && u == 4);
    CHECK(decode("\033[2~", &u) == TERM_KEY_INSERT && u == 4);
    CHECK(decode("\033[3~", &u) == TERM_KEY_DELETE && u == 4);
    CHECK(decode("\033[5~", &u) == TERM_KEY_PGUP && u == 4);
    CHECK(decode("\033[6~", &u) == TERM_KEY_PGDN && u == 4);

    // Modifiers ride in later parameters; the key is still the arrow.
    CHECK(decode("\033[1;5C", &u) == TERM_KEY_RIGHT && u == 6);

    // Only the start of a sequence: wait for more.
    CHECK(decode("\033", &u) == TERM_KEY_NONE && u == 0);
    CHECK(decode("\033[", &u) == TERM_KEY_NONE && u == 0);
    CHECK(decode("\033[5", &u) == TERM_KEY_NONE && u == 0);
    CHECK(decode("\033O", &u) == TERM_KEY_NONE && u == 0);

    // ESC before anything that is not a sequence is the Esc key, then that byte.
    CHECK(decode("\033x", &u) == TERM_KEY_ESC && u == 1);
    CHECK(decode("\033[\r", &u) == TERM_KEY_ESC && u == 1);

    // A whole sequence nobody maps is consumed, not returned.
    CHECK(decode("\033[Z", &u) == TERM_KEY_NONE && u == 3);

    // A sequence that never ends is dropped once it fills the buffer.
    CHECK(decode("\033[12345678901234", &u) == TERM_KEY_NONE && u == 16);
}

// term_present

static void present_twice(void)
{
    term_open();
    term_print(0, 0, "hi", TERM_NORMAL);
    term_present();
    write(1, "|", 1);
    term_present();
    write(1, "|", 1);
    term_put(0, 1, 'o', TERM_NORMAL);
    term_present();
    write(1, "|", 1);
    _exit(0);
}

static void test_term_present(void)
{
    char out[256];
    capture(present_twice, out, sizeof(out));

    char *first_end = strchr(out, '|');
    CHECK(first_end != NULL);
    if (!first_end) {
        return;
    }
    *first_end = '\0';
    char *second = first_end + 1;
    char *second_end = strchr(second, '|');
    CHECK(second_end != NULL);
    if (!second_end) {
        return;
    }
    *second_end = '\0';
    char *third = second_end + 1;
    char *third_end = strchr(third, '|');
    CHECK(third_end != NULL);
    if (third_end) {
        *third_end = '\0';
    }

    size_t len = strlen(out);
    CHECK(len >= 8 && strcmp(out + len - 8, "\033[1;1Hhi") == 0);
    CHECK(strcmp(second, "") == 0);
    CHECK(strcmp(third, "\033[1;2Ho") == 0);

    CHECK(term_print(0, TERM_COLS - 2, "abc", TERM_NORMAL) == 2);
}

// term size reports

static int size_parse(const char *s, int *rows, int *cols)
{
    return term_parse_size((const unsigned char *)s, (int)strlen(s), rows, cols);
}

// Runs child() with input on its stdin and its screen output in a pipe; 0 if it exited 0.
static int with_keys(const char *input, int (*child)(void))
{
    int in[2], out[2];
    if (pipe(in) != 0 || pipe(out) != 0) {
        return -1;
    }
    int pid = fork();
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        close(in[0]);
        close(in[1]);
        close(out[0]);
        close(out[1]);
        _exit(child());
    }
    close(in[0]);
    close(out[1]);
    write(in[1], input, strlen(input));
    close(in[1]);
    int status = -1;
    waitpid(pid, &status, 0);
    close(out[0]);
    return status;
}

static int size_reports_child(void)
{
    term_open();
    // Not a terminal, so nothing is asked and the screen keeps the default size.
    int ok = term_rows() == TERM_ROWS && term_cols() == TERM_COLS;
    ok = ok && term_key(500) == TERM_KEY_RESIZE && term_rows() == 30 && term_cols() == 100;
    ok = ok && term_key(500) == 'x';
    // The same size again changes nothing, so it never surfaces as a key.
    ok = ok && term_key(500) == TERM_KEY_EOF;
    term_close();
    return ok ? 0 : 1;
}

static void test_term_size_report(void)
{
    int r = 0, c = 0;
    CHECK(size_parse("\033[24;80R", &r, &c) == 8 && r == 24 && c == 80);
    CHECK(size_parse("\033[40;120Rx", &r, &c) == 9 && r == 40 && c == 120);

    // Only the start of one: wait for the rest.
    CHECK(size_parse("\033", &r, &c) == 0);
    CHECK(size_parse("\033[", &r, &c) == 0);
    CHECK(size_parse("\033[24", &r, &c) == 0);
    CHECK(size_parse("\033[24;8", &r, &c) == 0);

    // Not one at all.
    CHECK(size_parse("x", &r, &c) == -1);
    CHECK(size_parse("\033[A", &r, &c) == -1);
    CHECK(size_parse("\033[24R", &r, &c) == -1);
    CHECK(size_parse("\033[;80R", &r, &c) == -1);
    CHECK(size_parse("\033[24;R", &r, &c) == -1);
    CHECK(size_parse("\033[2a;3R", &r, &c) == -1);
    CHECK(size_parse("\033[123456;80R", &r, &c) == -1);
    // Shift-F3 has the same shape, but a screen is never one row tall.
    CHECK(size_parse("\033[1;2R", &r, &c) == -1);

    CHECK(with_keys("\033[30;100Rx\033[30;100R", size_reports_child) == 0);
}

// Entry point

static unsigned long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000 + (unsigned long)ts.tv_nsec / 1000000;
}

// A size near SIZE_MAX is refused at once, not after mapping chunk after chunk.
static void test_malloc_huge(void)
{
    unsigned long start = mono_ms();
    errno = 0;
    void *p = malloc(SIZE_MAX - 20);
    unsigned long took = mono_ms() - start;
    CHECK(p == NULL && errno == ENOMEM);
    CHECK(took < 100);
    CHECK(malloc(SIZE_MAX - (1u << 20)) == NULL);
    errno = 0;
    CHECK(calloc(SIZE_MAX / 2, 3) == NULL && errno == ENOMEM);
}

// Overwriting a variable frees the string the last setenv made, so a loop of them stays put.
static void test_setenv_no_leak(void)
{
    char *before = malloc(16);
    free(before);
    char value[41];
    for (int i = 0; i < 20000; i++) {
        snprintf(value, sizeof(value), "%040d", i);
        setenv("TLP_LEAK", value, 1);
    }
    char *after = malloc(16);
    free(after);
    CHECK(strcmp(getenv("TLP_LEAK"), "0000000000000000000000000000000000019999") == 0);
    CHECK((size_t)(after > before ? after - before : before - after) < 64 * 1024);
    unsetenv("TLP_LEAK");
}

// putenv matches the whole name, and unsetenv every copy of one.
static void test_putenv_unsetenv(void)
{
    static char n127[140], n200[220];
    memset(n127, 'N', 127);
    n127[127] = '\0';
    CHECK(setenv(n127, "keep", 1) == 0);
    memset(n200, 'N', 200);
    strcpy(n200 + 200, "=x");
    CHECK(putenv(n200) == 0);
    char *kept = getenv(n127);
    CHECK(kept && strcmp(kept, "keep") == 0);
    // putenv keeps n200 itself as the entry, so the name to look up is a copy.
    static char name200[201];
    memcpy(name200, n200, 200);
    char *got = getenv(name200);
    CHECK(got && strcmp(got, "x") == 0);
    unsetenv(name200);
    unsetenv(n127);

    char **saved = environ;
    char *dup[] = {"TLP_DUP=1", "TLP_OTHER=y", "TLP_DUP=2", NULL};
    environ = dup;
    CHECK(unsetenv("TLP_DUP") == 0);
    CHECK(getenv("TLP_DUP") == NULL);
    char *other = getenv("TLP_OTHER");
    CHECK(other && strcmp(other, "y") == 0);
    environ = saved;

    errno = 0;
    CHECK(setenv("A=B", "x", 1) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(putenv("noequals") == -1 && errno == EINVAL);
}

static void test_strtol(void)
{
    char *end;
    CHECK(atoi("+5") == 5);
    CHECK(atoi("\t7") == 7 && atoi("\n8") == 8 && atoi("  -12") == -12);
    CHECK(strtol("0x1f", &end, 0) == 31 && *end == '\0');
    CHECK(strtol("077", NULL, 0) == 63 && strtol("ff", NULL, 16) == 255);
    CHECK(strtol("  12abc", &end, 10) == 12 && strcmp(end, "abc") == 0);
    const char *none = "xyz";
    CHECK(strtol(none, &end, 10) == 0 && end == none);
    errno = 0;
    CHECK(strtol("99999999999999999999", NULL, 10) == LONG_MAX && errno == ERANGE);
    errno = 0;
    CHECK(strtol("-9223372036854775808", NULL, 10) == LONG_MIN && errno == 0);
    errno = 0;
    CHECK(strtol("-9223372036854775809", NULL, 10) == LONG_MIN && errno == ERANGE);
    CHECK(strtoul("-1", NULL, 10) == ULONG_MAX);
    errno = 0;
    CHECK(strtoul("18446744073709551616", NULL, 10) == ULONG_MAX && errno == ERANGE);
    CHECK(abs(-3) == 3 && labs(-4L) == 4L);
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}

static void test_bsearch(void)
{
    static const int sorted[] = {1, 3, 5, 7, 9, 11};
    for (int i = 0; i < 6; i++) {
        const int *hit = bsearch(&sorted[i], sorted, 6, sizeof(int), cmp_int);
        CHECK(hit == &sorted[i]);
    }
    int missing = 4;
    CHECK(bsearch(&missing, sorted, 6, sizeof(int), cmp_int) == NULL);
    CHECK(bsearch(&missing, sorted, 0, sizeof(int), cmp_int) == NULL);
}

// execvp stops at the first file it finds that will not run, with that errno.
static void test_execvp_errors(void)
{
    mkdir("/tlp_dir", 0755);
    int fd = open("/tlp_dir/sleep.elf", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd >= 0) {
        write(fd, "not an ELF\n", 11);
        close(fd);
    }
    int pid = fork();
    if (pid == 0) {
        setenv("PATH", "/tlp_dir:/bin", 1);
        execvp("sleep", (char *[]){"sleep", "0", NULL});
        _exit(errno == ENOEXEC ? 42 : 1);
    }
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 42);
    unlink("/tlp_dir/sleep.elf");
    rmdir("/tlp_dir");

    errno = 0;
    CHECK(execvp("tlp_no_such_program", (char *[]){"x", NULL}) == -1 && errno == ENOENT);
}

static void on_abort(int sig)
{
    (void)sig;
}

// abort ends the program even when a handler for SIGABRT returns.
static void test_abort(void)
{
    for (int handled = 0; handled < 2; handled++) {
        int pid = fork();
        if (pid == 0) {
            if (handled) {
                signal(SIGABRT, on_abort);
            }
            abort();
        }
        int st = 0;
        CHECK(waitpid(pid, &st, 0) == pid && WEXITSTATUS(st) == 128 + SIGABRT);
    }
}

// The stdio calls POSIX code expects: line reads, a pushed-back byte, puts and setvbuf.
static void test_stdio_lines(void)
{
    int p[2];
    CHECK(pipe(p) == 0);
    int x = 0x0a6463; // "cd\n" as bytes, through a const void * write
    CHECK(write(p[1], "ab\n", 3) == 3 && write(p[1], &x, 2) == 2);
    close(p[1]);

    FILE in = {.fd = p[0]};
    char line[8];
    CHECK(fgets(line, sizeof(line), &in) && strcmp(line, "ab\n") == 0);
    int c = fgetc(&in);
    CHECK(c == 'c' && ungetc(c, &in) == 'c' && ungetc('z', &in) == EOF);
    CHECK(fgets(line, sizeof(line), &in) && strcmp(line, "cd") == 0);
    CHECK(fgets(line, sizeof(line), &in) == NULL && feof(&in));
    close(p[0]);

    CHECK(setvbuf(stdout, NULL, _IOFBF, BUFSIZ) == 0);
    CHECK(fputs("", stdout) == 0 && puts("[stdio] puts writes a line") == 0);
    CHECK(setvbuf(stdout, NULL, 99, 0) == -1);
}

int main(int argc, char **argv)
{
    // Child of test_atexit: the handlers must run on the return from main.
    if (argc > 1 && strcmp(argv[1], "--atexit-child") == 0) {
        atexit(mark_a);
        atexit(mark_b);
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "--stdout-child") == 0) {
        return stdout_child(argv[2]);
    }

    printf("[TEST] libc integrity test suite\n");

    run_group("assert", test_assert);
    run_group("strlen", test_strlen);
    run_group("strcpy", test_strcpy);
    run_group("strncpy", test_strncpy);
    run_group("strcat", test_strcat);
    run_group("strncat", test_strncat);
    run_group("strcmp", test_strcmp);
    run_group("strncmp", test_strncmp);
    run_group("strchr", test_strchr);
    run_group("strrchr", test_strrchr);
    run_group("strstr", test_strstr);
    run_group("strtok_r", test_strtok_r);
    run_group("strnlen", test_strnlen);
    run_group("strdup", test_strdup);
    run_group("strndup", test_strndup);
    run_group("memset", test_memset);
    run_group("memcpy", test_memcpy);
    run_group("memmove", test_memmove);
    run_group("malloc/free", test_malloc_basic);
    run_group("malloc(0)", test_malloc_zero);
    run_group("malloc multi", test_malloc_multiple);
    run_group("malloc coalesce", test_malloc_coalesce);
    run_group("calloc", test_calloc);
    run_group("realloc", test_realloc);
    run_group("isalnum", test_isalnum);
    run_group("isalpha", test_isalpha);
    run_group("isupper", test_isupper);
    run_group("islower", test_islower);
    run_group("isdigit", test_isdigit);
    run_group("isspace", test_isspace);
    run_group("isblank", test_isblank);
    run_group("ispunct", test_ispunct);
    run_group("iscntrl", test_iscntrl);
    run_group("isxdigit", test_isxdigit);
    run_group("isgraph", test_isgraph);
    run_group("isprint", test_isprint);
    run_group("toupper", test_toupper);
    run_group("tolower", test_tolower);
    run_group("ctype bounds", test_ctype_bounds);
    run_group("snprintf basic", test_snprintf_basic);
    run_group("snprintf bounds", test_snprintf_bounds);
    run_group("snprintf %%", test_snprintf_percent);
    run_group("snprintf sizing", test_snprintf_sizing);
    run_group("snprintf precision", test_snprintf_precision);
    run_group("snprintf #", test_snprintf_hash);
    run_group("snprintf length modifiers", test_snprintf_lengths);
    run_group("stdout survives EINTR", test_stdout_survives_eintr);
    run_group("fread waits for a pipe", test_fread_waits_for_pipe);
    run_group("fclose flushes stdout", test_fclose_flushes_stdout);
    run_group("setjmp direct", test_setjmp_direct);
    run_group("longjmp value", test_longjmp_value);
    run_group("longjmp zero", test_longjmp_zero);
    run_group("longjmp nested", test_longjmp_nested);
    run_group("setjmp independent", test_setjmp_independent);
    run_group("errno ENOENT", test_errno_open);
    run_group("errno EBADF", test_errno_close);
    run_group("errno preserved", test_errno_preserved);
    run_group("strerror known", test_strerror_known);
    run_group("strerror aliases", test_strerror_aliases);
    run_group("strerror gaps", test_strerror_gaps);
    run_group("strerror out of range", test_strerror_out_of_range);
    run_group("strerror non-null", test_strerror_nonnull);
    run_group("strerror keying", test_strerror_keying);
    run_group("strerror end-to-end", test_strerror_end_to_end);
    run_group("qsort", test_qsort);
    run_group("rand", test_rand);
    run_group("atexit", test_atexit);
    run_group("stdout buffering", test_stdout_buffering);
    run_group("term_decode", test_term_decode);
    run_group("term_present", test_term_present);
    run_group("term size reports", test_term_size_report);
    run_group("malloc near SIZE_MAX", test_malloc_huge);
    run_group("setenv frees what it replaces", test_setenv_no_leak);
    run_group("putenv and unsetenv", test_putenv_unsetenv);
    run_group("strtol and atoi", test_strtol);
    run_group("bsearch", test_bsearch);
    run_group("execvp errors", test_execvp_errors);
    run_group("abort", test_abort);
    run_group("stdio lines", test_stdio_lines);

    if (g_failed) {
        printf("test_libc: %d of %d tests failed\n", g_failed, g_passed + g_failed);
        return 1;
    }
    printf("test_libc: all %d tests passed\n", g_passed);
    return 0;
}
