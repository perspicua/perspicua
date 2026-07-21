/*
 * test_libc.c - User-space libc integrity test suite.
 *
 * Covers: string functions, malloc/free/realloc, printf/snprintf, ctype
 * classification/case-conversion, and errno.
 * Each test group prints a PASS/FAIL summary line. Individual check failures
 * print the source location so regressions are easy to pinpoint.
 *
 * Exit status: 0 if all checks pass, 1 if any fail.
 */

#include "assert.h"
#include "ctype.h"
#include "errno.h"
#include "setjmp.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "syscall.h"

/* --- Minimal test framework -------------------------------------------- */

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

/* Run fn(), report PASS if no new failures occurred. */
static void run_group(const char *name, void (*fn)(void))
{
    int before = g_failed;
    fn();
    if (g_failed == before)
        printf("[PASS] %s\n", name);
    else
        printf("[FAIL] %s (%d check(s) failed)\n", name, g_failed - before);
}

/* --- assert() smoke test ----------------------------------------------- */

static void test_assert(void)
{
    /* assert on a true condition must not abort. */
    assert(1 == 1);
    assert(0 == 0);
    assert((void *)0 == NULL);
    CHECK(1); /* Reached only if the asserts above did not abort. */

    /* assert(0) would call __assert_fail and exit — not tested here. */
}

/* --- string.h ---------------------------------------------------------- */

static void test_strlen(void)
{
    CHECK(strlen("") == 0);
    CHECK(strlen("a") == 1);
    CHECK(strlen("hello") == 5);
    CHECK(strlen("hello\0world") == 5); /* Stops at first NUL. */
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
    CHECK(buf[2] == '\0'); /* Rest padded with NULs. */
    CHECK(buf[7] == '\0');

    /* Truncation: no NUL appended when src >= count. */
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
    CHECK(strncmp("abcX", "abcY", 3) == 0); /* First 3 chars match. */
    CHECK(strncmp("abcX", "abcY", 4) != 0);
    CHECK(strncmp("abc", "abc", 0) == 0); /* Zero count always equal. */
}

static void test_strchr(void)
{
    const char *s = "hello";
    CHECK(strchr(s, 'e') == s + 1);
    CHECK(strchr(s, 'z') == NULL);
    CHECK(strchr(s, '\0') == s + 5); /* strchr finds the NUL terminator. */
}

static void test_strrchr(void)
{
    const char *s = "hello";
    CHECK(strrchr(s, 'l') == s + 3); /* Last 'l' is at index 3. */
    CHECK(strrchr(s, 'z') == NULL);
}

static void test_strstr(void)
{
    CHECK(strstr("foobar", "bar") != NULL);
    CHECK(strstr("foobar", "baz") == NULL);
    CHECK(strstr("foobar", "") != NULL); /* Empty needle returns haystack. */
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

    tok = strtok_r(NULL, ",", &save); /* Empty field skipped. */
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

    /* Unaligned sizes. */
    char d3[3];
    memcpy(d3, "xyz", 3);
    CHECK(d3[0] == 'x' && d3[1] == 'y' && d3[2] == 'z');
}

static void test_strnlen(void)
{
    CHECK(strnlen("abc", 10) == 3); /* Terminator found before the limit. */
    CHECK(strnlen("abc", 3) == 3);
    CHECK(strnlen("abcdef", 3) == 3); /* Limit reached first. */
    CHECK(strnlen("abc", 0) == 0);
    CHECK(strnlen("", 5) == 0);
    CHECK(strnlen(NULL, 5) == 0);

    /* No terminator within the limit: must stop at n, not scan past the end. */
    char unterminated[4] = {'a', 'b', 'c', 'd'};
    CHECK(strnlen(unterminated, 4) == 4);
}

static void test_strdup(void)
{
    const char *src = "duplicate me";
    char *dup = strdup(src);
    CHECK(dup != NULL);
    CHECK(strcmp(dup, src) == 0);
    CHECK(dup != src); /* A copy, not the same storage. */
    dup[0] = 'D';
    CHECK(src[0] == 'd'); /* Writing the copy must not touch the source. */
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

    char *whole = strndup("abc", 10); /* n longer than the source. */
    CHECK(whole != NULL && strcmp(whole, "abc") == 0);
    free(whole);

    char *none = strndup("abc", 0);
    CHECK(none != NULL && strlen(none) == 0);
    free(none);

    /* Must terminate at n even when the source has no NUL in range. */
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
    /* Forward overlap: src < dst. */
    char buf[16] = "abcdefgh";
    memmove(buf + 2, buf, 6); /* "ab" -> "ababcdef" at [2..7]. */
    CHECK(buf[2] == 'a' && buf[7] == 'f');

    /* Backward overlap: dst < src. */
    char buf2[16] = "abcdefgh";
    memmove(buf2, buf2 + 2, 6);
    CHECK(buf2[0] == 'c' && buf2[5] == 'h');
}

/* --- malloc / free / realloc ------------------------------------------- */

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
    /* malloc(0) may return NULL or a unique pointer; must not crash. */
    void *p = malloc(0);
    free(p);  /* Freeing NULL is always safe. */
    CHECK(1); /* Reached without crash. */
}

static void test_malloc_multiple(void)
{
    /* Allocate several blocks and verify they don't overlap. */
    const int N = 8;
    void *ptrs[8];
    for (int i = 0; i < N; i++) {
        ptrs[i] = malloc(128);
        CHECK(ptrs[i] != NULL);
        memset(ptrs[i], (unsigned char)i, 128);
    }
    /* Verify no block was overwritten by a subsequent allocation. */
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
    /* Allocate something that fits in the coalesced region. */
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
    /* Grow an allocation. */
    char *p = malloc(32);
    CHECK(p != NULL);
    memset(p, 'A', 32);
    p = realloc(p, 128);
    CHECK(p != NULL);
    /* Original bytes must be preserved. */
    int ok = 1;
    for (int i = 0; i < 32; i++) {
        if (p[i] != 'A') {
            ok = 0;
            break;
        }
    }
    CHECK(ok);
    free(p);

    /* realloc(NULL, size) must behave like malloc. */
    char *q = realloc(NULL, 64);
    CHECK(q != NULL);
    free(q);

    /* realloc(ptr, 0) must behave like free. */
    char *r = malloc(16);
    CHECK(r != NULL);
    void *res = realloc(r, 0);
    CHECK(res == NULL); /* Returns NULL after freeing. */
}

/* --- printf / snprintf ------------------------------------------------- */

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

    /* Output must be truncated and always NUL-terminated. */
    int n = snprintf(buf, sizeof(buf), "hello world");
    CHECK(buf[4] == '\0'); /* Always NUL-terminated. */
    CHECK(strncmp(buf, "hell", 4) == 0);
    CHECK(n == 11); /* Returns the would-be length. */

    /* Minimal buffer: size=1 means only the NUL fits. */
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

/* --- ctype.h ------------------------------------------------------------ */

static void test_isalnum(void)
{
    CHECK(isalnum('a') && isalnum('z') && isalnum('A') && isalnum('Z'));
    CHECK(isalnum('0') && isalnum('9'));
    CHECK(!isalnum(' ') && !isalnum('!') && !isalnum('\n') && !isalnum('_'));
}

static void test_isalpha(void)
{
    CHECK(isalpha('a') && isalpha('Z'));
    CHECK(!isalpha('0') && !isalpha('9')); /* Digits are not alpha. */
    CHECK(!isalpha(' ') && !isalpha('.'));
}

static void test_isupper(void)
{
    CHECK(isupper('A') && isupper('M') && isupper('Z'));
    CHECK(!isupper('a') && !isupper('z')); /* Lowercase is not upper. */
    CHECK(!isupper('5') && !isupper(' ') && !isupper('!'));
}

static void test_islower(void)
{
    CHECK(islower('a') && islower('m') && islower('z'));
    CHECK(!islower('A') && !islower('Z')); /* Uppercase is not lower. */
    CHECK(!islower('5') && !islower(' ') && !islower('!'));
}

static void test_isdigit(void)
{
    for (char c = '0'; c <= '9'; c++) {
        CHECK(isdigit(c));
    }
    CHECK(!isdigit('a') && !isdigit(' ') && !isdigit('/') && !isdigit(':'));
    /* '/' and ':' are the bytes immediately outside the '0'-'9' range. */
}

static void test_isspace(void)
{
    CHECK(isspace(' ') && isspace('\t') && isspace('\n'));
    CHECK(isspace('\v') && isspace('\f') && isspace('\r'));
    CHECK(!isspace('a') && !isspace('0'));
}

static void test_isblank(void)
{
    /* isblank is narrower than isspace: only space and tab. */
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
    CHECK(iscntrl('\0') && iscntrl('\n') && iscntrl(0x1F) && iscntrl(0x7F)); /* DEL */
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
    /* isgraph: any printable character EXCEPT space. */
    CHECK(isgraph('a') && isgraph('Z') && isgraph('5'));
    CHECK(isgraph('!') && isgraph('~')); /* Punctuation counts. */
    CHECK(!isgraph(' '));                /* Space is excluded. */
    CHECK(!isgraph('\t') && !isgraph('\n') && !isgraph(0x1F) && !isgraph(0x7F));
}

static void test_isprint(void)
{
    /* isprint: any printable character INCLUDING space. */
    CHECK(isprint('a') && isprint('Z') && isprint('5'));
    CHECK(isprint('!') && isprint('~'));
    CHECK(isprint(' ')); /* The one difference from isgraph. */
    CHECK(!isprint('\t') && !isprint('\n') && !isprint(0x1F) && !isprint(0x7F));
}

static void test_toupper(void)
{
    CHECK(toupper('a') == 'A');
    CHECK(toupper('z') == 'Z');
    CHECK(toupper('A') == 'A'); /* Already upper: unchanged. */
    CHECK(toupper('5') == '5'); /* Non-letter: unchanged. */
    CHECK(toupper('!') == '!');
}

static void test_tolower(void)
{
    CHECK(tolower('A') == 'a');
    CHECK(tolower('Z') == 'z');
    CHECK(tolower('a') == 'a'); /* Already lower: unchanged. */
    CHECK(tolower('5') == '5'); /* Non-letter: unchanged. */
    CHECK(tolower('!') == '!');
}

static void test_ctype_bounds(void)
{
    /* EOF and out-of-range values must be handled without indexing OOB. */
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

/* --- setjmp / longjmp --------------------------------------------------- */

static jmp_buf g_basic_jb;
static jmp_buf g_zero_jb;
static jmp_buf g_nest_jb;

static void test_setjmp_direct(void)
{
    jmp_buf local;
    /* A setjmp with no matching longjmp reports the first-time return. */
    CHECK(setjmp(local) == 0);
}

static void test_longjmp_value(void)
{
    volatile int before_jump = 0;
    int rc = setjmp(g_basic_jb);

    if (rc == 0) {
        before_jump = 1;
        longjmp(g_basic_jb, 42);
        /* Unreached: falling through here would leave rc at 0 below. */
    }

    CHECK(rc == 42);         /* setjmp returned longjmp's value */
    CHECK(before_jump == 1); /* volatile local survived the jump */
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
            CHECK(0); /* longjmp(buf, 0) kept returning 0 */
            return;
        }
        longjmp(g_zero_jb, 0);
    }

    CHECK(rc == 1);
}

/* Jumps out of three nested frames at once. */
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

/* Recurses so a wrong sp after longjmp shows up as a wrong sum or a fault. */
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
    /* The abandoned frames must leave a usable stack behind. */
    CHECK(sum_to(10) == 55);
}

static void test_setjmp_independent(void)
{
    /* Two buffers must not alias: jumping through one leaves the other set. */
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

/* --- strerror ----------------------------------------------------------- */

static void test_strerror_keying(void)
{
    /* Guards the numbering collision: the internal PERS_ERR_IO_ERROR is 9 and
     * so is EBADF, so a table keyed on the internal codes would return the
     * I/O-error text for value 9. These must stay distinct and correct. */
    CHECK(EBADF == 9);
    CHECK(strcmp(strerror(9), "Bad file descriptor") == 0);
    CHECK(strcmp(strerror(EIO), "I/O error") == 0);
    CHECK(strcmp(strerror(EIO), strerror(EBADF)) != 0);

    /* PERS_ERR_NOT_FOUND is 1 while ENOENT is 2 — an off-by-one keying would
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
    int fd = sys_open("/no/such/file", VFS_O_RDONLY);
    CHECK(fd == -1);
    CHECK(strcmp(strerror(errno), "No such file or directory") == 0);

    errno = 0;
    int ret = sys_close(9999);
    CHECK(ret == -1);
    CHECK(strcmp(strerror(errno), "Bad file descriptor") == 0);
}

/* --- errno ------------------------------------------------------------- */

static void test_errno_open(void)
{
    /* Opening a non-existent file must set errno to ENOENT. */
    errno = 0;
    int fd = sys_open("/no/such/file", VFS_O_RDONLY);
    CHECK(fd == -1);
    CHECK(errno == ENOENT);
}

static void test_errno_close(void)
{
    /* Closing an invalid descriptor must set errno to EBADF. */
    errno = 0;
    int ret = sys_close(9999);
    CHECK(ret == -1);
    CHECK(errno == EBADF);
}

static void test_errno_preserved(void)
{
    /* A successful call must not disturb errno. */
    errno = EINVAL;
    int pid = sys_getpid();
    CHECK(pid > 0);
    CHECK(errno == EINVAL); /* Unchanged by the successful call. */
}

/* --- strerror ------------------------------------------------------------ */

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

    /* Negative values. */
    snprintf(expected, sizeof(expected), "Unknown error %d", -1);
    CHECK(strcmp(strerror(-1), expected) == 0);

    /* Just past the highest defined code (ECONNREFUSED == 111). */
    snprintf(expected, sizeof(expected), "Unknown error %d", 112);
    CHECK(strcmp(strerror(112), expected) == 0);

    /* Far out of range, well beyond any lookup table bound. */
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

/* --- Entry point ------------------------------------------------------- */

int main(void)
{
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

    printf("\n[RESULT] %d passed, %d failed\n", g_passed, g_failed);

    return g_failed > 0 ? 1 : 0;
}
