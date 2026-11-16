/*
 * host_libc_test.c - Compares libc/src with the host's libc over generated inputs.
 *
 * host_libc_test.sh builds the library with every public name prefixed p_; this file calls both
 * and reports where they differ. The generators stay inside what C defines, as the two libcs
 * are free to differ elsewhere.
 */

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int p_snprintf(char *buf, size_t size, const char *fmt, ...);
long p_strtol(const char *s, char **end, int base);
unsigned long p_strtoul(const char *s, char **end, int base);
int p_atoi(const char *s);
void *p_memcpy(void *dst, const void *src, size_t n);
void *p_memmove(void *dst, const void *src, size_t n);
void *p_memset(void *dst, int c, size_t n);
int p_memcmp(const void *a, const void *b, size_t n);
int p_strncmp(const char *a, const char *b, size_t n);
char *p_strstr(const char *hay, const char *needle);
size_t p_strlen(const char *s);
size_t p_strspn(const char *s, const char *accept);
size_t p_strcspn(const char *s, const char *reject);
char *p_strncat(char *dst, const char *src, size_t n);
char *p_strncpy(char *dst, const char *src, size_t n);
void p_qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));

#define CTYPE_LIST(X) \
    X(isalnum)        \
    X(isalpha)        \
    X(isblank)        \
    X(iscntrl)        \
    X(isdigit)        \
    X(isgraph)        \
    X(islower)        \
    X(isprint)        \
    X(ispunct)        \
    X(isspace)        \
    X(isupper)        \
    X(isxdigit)       \
    X(tolower)        \
    X(toupper)

#define DECLARE(name) int p_##name(int c);
CTYPE_LIST(DECLARE)

int p_errno;

void p___libc_write(const char *buf, size_t len)
{
    (void)buf;
    (void)len;
}

static unsigned long cases, mismatches;

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static uint64_t rnd(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1DULL;
}

static unsigned pick(unsigned n)
{
    return (unsigned)(rnd() % n);
}

static void mismatch(const char *what, const char *detail)
{
    if (++mismatches <= 20) {
        printf("MISMATCH %s: %s\n", what, detail);
    }
}

// Values that sit on the edges of every width, then random ones of every size.
static uint64_t pick_value(void)
{
    static const uint64_t edge[] = {0,
                                    1,
                                    7,
                                    8,
                                    9,
                                    15,
                                    255,
                                    256,
                                    0x7FFFFFFFULL,
                                    0x80000000ULL,
                                    0xFFFFFFFFULL,
                                    0x7FFFFFFFFFFFFFFFULL,
                                    0x8000000000000000ULL,
                                    0xFFFFFFFFFFFFFFFFULL};
    unsigned k = pick(4);
    if (k == 0) {
        return edge[pick(sizeof(edge) / sizeof(edge[0]))];
    }
    return rnd() >> pick(64);
}

enum {
    L_NONE,
    L_HH,
    L_H,
    L_L,
    L_LL,
    L_Z,
    L_J,
    L_T,
    L_COUNT
};
static const char *const length_text[L_COUNT] = {"", "hh", "h", "l", "ll", "z", "j", "t"};

#define CALL(fn, buf, size, fmt, kind, v, is_signed)                               \
    ((is_signed) ? ((kind) == L_L    ? fn(buf, size, fmt, (long)(v))               \
                    : (kind) == L_LL ? fn(buf, size, fmt, (long long)(v))          \
                    : (kind) == L_Z  ? fn(buf, size, fmt, (ssize_t)(v))            \
                    : (kind) == L_J  ? fn(buf, size, fmt, (intmax_t)(v))           \
                    : (kind) == L_T  ? fn(buf, size, fmt, (ptrdiff_t)(v))          \
                                     : fn(buf, size, fmt, (int)(v)))               \
                 : ((kind) == L_L    ? fn(buf, size, fmt, (unsigned long)(v))      \
                    : (kind) == L_LL ? fn(buf, size, fmt, (unsigned long long)(v)) \
                    : (kind) == L_Z  ? fn(buf, size, fmt, (size_t)(v))             \
                    : (kind) == L_J  ? fn(buf, size, fmt, (uintmax_t)(v))          \
                    : (kind) == L_T  ? fn(buf, size, fmt, (ptrdiff_t)(v))          \
                                     : fn(buf, size, fmt, (unsigned int)(v))))

static void test_snprintf(void)
{
    static const char convs[] = "diuxXocs";
    static const size_t sizes[] = {0, 1, 5, 64};
    static const char *const strings[] = {"", "a", "hello", "0123456789abcdef"};

    for (int i = 0; i < 400000; i++) {
        char conv = convs[pick(sizeof(convs) - 1)];
        int is_signed = conv == 'd' || conv == 'i';
        int numeric = conv != 'c' && conv != 's';
        int kind = numeric ? (int)pick(L_COUNT) : L_NONE;

        char fmt[32];
        char *f = fmt;
        *f++ = '%';
        if (pick(3) == 0) {
            *f++ = '-';
        }
        if (is_signed && pick(3) == 0) {
            *f++ = pick(2) ? '+' : ' ';
        }
        if ((conv == 'x' || conv == 'X' || conv == 'o') && pick(2) == 0) {
            *f++ = '#';
        }
        if (numeric && pick(3) == 0) {
            *f++ = '0';
        }
        if (pick(2) == 0) {
            f += snprintf(f, 4, "%u", pick(14));
        }
        if (conv != 'c' && pick(2) == 0) {
            f += snprintf(f, 5, ".%u", pick(14));
        }
        f += snprintf(f, 3, "%s", length_text[kind]);
        *f++ = conv;
        *f = '\0';

        size_t size = sizes[pick(sizeof(sizes) / sizeof(sizes[0]))];
        char want[96], got[96];
        memset(want, 0x55, sizeof(want));
        memset(got, 0x55, sizeof(got));

        uint64_t v = pick_value();
        const char *str = strings[pick(sizeof(strings) / sizeof(strings[0]))];
        int want_n, got_n;
        if (conv == 's') {
            want_n = snprintf(want, size, fmt, str);
            got_n = p_snprintf(got, size, fmt, str);
        } else if (conv == 'c') {
            want_n = snprintf(want, size, fmt, (int)('a' + v % 26));
            got_n = p_snprintf(got, size, fmt, (int)('a' + v % 26));
        } else {
            want_n = CALL(snprintf, want, size, fmt, kind, v, is_signed);
            got_n = CALL(p_snprintf, got, size, fmt, kind, v, is_signed);
        }

        cases++;
        if (want_n != got_n || memcmp(want, got, sizeof(want)) != 0) {
            char detail[160];
            snprintf(detail, sizeof(detail),
                     "\"%s\" size %zu value %" PRIu64 ": want \"%s\" (%d), got \"%s\" (%d)", fmt,
                     size, v, want, want_n, got, got_n);
            mismatch("snprintf", detail);
        }
    }
}

static void random_text(char *out, size_t max, const char *alphabet)
{
    size_t n = pick((unsigned)max);
    size_t len = strlen(alphabet);
    for (size_t i = 0; i < n; i++) {
        out[i] = alphabet[pick((unsigned)len)];
    }
    out[n] = '\0';
}

static void test_strtol(void)
{
    static const int bases[] = {0, 2, 8, 10, 16, 36};
    for (int i = 0; i < 100000; i++) {
        char text[40];
        random_text(text, sizeof(text) - 1, "0123456789abcdefxXzZ+- \t");
        int base = bases[pick(sizeof(bases) / sizeof(bases[0]))];
        char *want_end, *got_end;

        errno = 0;
        p_errno = 0;
        long want = strtol(text, &want_end, base);
        long got = p_strtol(text, &got_end, base);
        int want_range = errno == ERANGE, got_range = p_errno == 34;
        cases++;
        if (want != got || want_end - text != got_end - text || want_range != got_range) {
            char detail[160];
            snprintf(detail, sizeof(detail),
                     "\"%s\" base %d: want %ld (+%ld, range %d), got %ld (+%ld, range %d)", text,
                     base, want, (long)(want_end - text), want_range, got, (long)(got_end - text),
                     got_range);
            mismatch("strtol", detail);
        }

        errno = 0;
        p_errno = 0;
        unsigned long uwant = strtoul(text, &want_end, base);
        unsigned long ugot = p_strtoul(text, &got_end, base);
        want_range = errno == ERANGE;
        got_range = p_errno == 34;
        cases++;
        if (uwant != ugot || want_end - text != got_end - text || want_range != got_range) {
            char detail[160];
            snprintf(detail, sizeof(detail),
                     "\"%s\" base %d: want %lu (+%ld, range %d), got %lu (+%ld, range %d)", text,
                     base, uwant, (long)(want_end - text), want_range, ugot, (long)(got_end - text),
                     got_range);
            mismatch("strtoul", detail);
        }

        cases++;
        if (atoi(text) != p_atoi(text)) {
            char detail[96];
            snprintf(detail, sizeof(detail), "\"%s\": want %d, got %d", text, atoi(text),
                     p_atoi(text));
            mismatch("atoi", detail);
        }
    }
}

static int sign(int v)
{
    return (v > 0) - (v < 0);
}

static void test_memory(void)
{
    for (int i = 0; i < 100000; i++) {
        unsigned char a[96], b[96];
        for (size_t k = 0; k < sizeof(a); k++) {
            a[k] = b[k] = (unsigned char)rnd();
        }
        size_t dst = pick(16), src = pick(48), n = pick(40);

        memmove(a + dst, a + src, n);
        p_memmove(b + dst, b + src, n);
        cases++;
        if (memcmp(a, b, sizeof(a)) != 0) {
            char detail[96];
            snprintf(detail, sizeof(detail), "dst %zu src %zu n %zu", dst, src, n);
            mismatch("memmove", detail);
        }

        unsigned char c[96], d[96];
        for (size_t k = 0; k < sizeof(c); k++) {
            c[k] = d[k] = (unsigned char)rnd();
        }
        unsigned char from[96];
        for (size_t k = 0; k < sizeof(from); k++) {
            from[k] = (unsigned char)rnd();
        }
        memcpy(c + dst, from + src, n);
        p_memcpy(d + dst, from + src, n);
        cases++;
        if (memcmp(c, d, sizeof(c)) != 0) {
            char detail[96];
            snprintf(detail, sizeof(detail), "dst %zu src %zu n %zu", dst, src, n);
            mismatch("memcpy", detail);
        }

        int fill = (int)rnd();
        memset(c + dst, fill, n);
        p_memset(d + dst, fill, n);
        cases++;
        if (memcmp(c, d, sizeof(c)) != 0) {
            mismatch("memset", "contents differ");
        }

        size_t m = pick(40);
        cases++;
        if (sign(memcmp(a, from, m)) != sign(p_memcmp(a, from, m))) {
            mismatch("memcmp", "sign differs");
        }
    }
}

static void test_strings(void)
{
    for (int i = 0; i < 100000; i++) {
        char s[16], t[16], needle[6], set[6];
        random_text(s, sizeof(s) - 1, "abc ");
        random_text(t, sizeof(t) - 1, "abc ");
        random_text(needle, sizeof(needle) - 1, "abc");
        random_text(set, sizeof(set) - 1, "abc");
        size_t n = pick(20);

        cases++;
        if (strlen(s) != p_strlen(s)) {
            mismatch("strlen", s);
        }
        cases++;
        if (sign(strncmp(s, t, n)) != sign(p_strncmp(s, t, n))) {
            char detail[64];
            snprintf(detail, sizeof(detail), "\"%s\" \"%s\" n %zu", s, t, n);
            mismatch("strncmp", detail);
        }
        const char *want_at = strstr(s, needle), *got_at = p_strstr(s, needle);
        cases++;
        if ((want_at == NULL) != (got_at == NULL) || (want_at && want_at - s != got_at - s)) {
            char detail[64];
            snprintf(detail, sizeof(detail), "\"%s\" in \"%s\"", needle, s);
            mismatch("strstr", detail);
        }
        cases++;
        if (strspn(s, set) != p_strspn(s, set) || strcspn(s, set) != p_strcspn(s, set)) {
            char detail[64];
            snprintf(detail, sizeof(detail), "\"%s\" set \"%s\"", s, set);
            mismatch("strspn/strcspn", detail);
        }

        char want[48], got[48];
        memset(want, 0x55, sizeof(want));
        memcpy(want, s, strlen(s) + 1);
        memcpy(got, want, sizeof(got));
        strncat(want, t, n);
        p_strncat(got, t, n);
        cases++;
        if (memcmp(want, got, sizeof(want)) != 0) {
            char detail[96];
            snprintf(detail, sizeof(detail), "\"%s\" + \"%s\" n %zu", s, t, n);
            mismatch("strncat", detail);
        }

        memset(want, 0x55, sizeof(want));
        memset(got, 0x55, sizeof(got));
        strncpy(want, t, n);
        p_strncpy(got, t, n);
        cases++;
        if (memcmp(want, got, sizeof(want)) != 0) {
            char detail[64];
            snprintf(detail, sizeof(detail), "\"%s\" n %zu", t, n);
            mismatch("strncpy", detail);
        }
    }
}

static int int_cmp(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static void test_qsort(void)
{
    for (int i = 0; i < 20000; i++) {
        int want[48], got[48];
        size_t n = pick(48);
        for (size_t k = 0; k < n; k++) {
            want[k] = got[k] = (int)pick(20) - 10;
        }
        qsort(want, n, sizeof(int), int_cmp);
        p_qsort(got, n, sizeof(int), int_cmp);
        cases++;
        if (memcmp(want, got, n * sizeof(int)) != 0) {
            mismatch("qsort", "order differs");
        }
    }
}

static void test_ctype(void)
{
#define COMPARE(name)                                                                      \
    for (int c = -1; c < 256; c++) {                                                       \
        cases++;                                                                           \
        int want = name(c), got = p_##name(c);                                             \
        if ((#name[0] == 't') ? want != got : (want != 0) != (got != 0)) {                 \
            char detail[48];                                                               \
            snprintf(detail, sizeof(detail), #name "(%d): want %d, got %d", c, want, got); \
            mismatch("ctype", detail);                                                     \
        }                                                                                  \
    }
    CTYPE_LIST(COMPARE)
}

int main(void)
{
    test_snprintf();
    test_strtol();
    test_memory();
    test_strings();
    test_qsort();
    test_ctype();

    printf("host_libc: %lu cases, %lu mismatches\n", cases, mismatches);
    return mismatches ? 1 : 0;
}
