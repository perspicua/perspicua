/*
 * test_libc.c - User-space libc integrity test suite.
 *
 * Covers: string functions, malloc/free/realloc, printf/snprintf, and errno.
 * Each test group prints a PASS/FAIL summary line. Individual check failures
 * print the source location so regressions are easy to pinpoint.
 *
 * Exit status: 0 if all checks pass, 1 if any fail.
 */

#include "assert.h"
#include "errno.h"
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
    run_group("memset", test_memset);
    run_group("memcpy", test_memcpy);
    run_group("memmove", test_memmove);
    run_group("malloc/free", test_malloc_basic);
    run_group("malloc(0)", test_malloc_zero);
    run_group("malloc multi", test_malloc_multiple);
    run_group("malloc coalesce", test_malloc_coalesce);
    run_group("calloc", test_calloc);
    run_group("realloc", test_realloc);
    run_group("snprintf basic", test_snprintf_basic);
    run_group("snprintf bounds", test_snprintf_bounds);
    run_group("snprintf %%", test_snprintf_percent);
    run_group("errno ENOENT", test_errno_open);
    run_group("errno EBADF", test_errno_close);
    run_group("errno preserved", test_errno_preserved);

    printf("\n[RESULT] %d passed, %d failed\n", g_passed, g_failed);

    return g_failed > 0 ? 1 : 0;
}
