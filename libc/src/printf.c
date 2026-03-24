/*
 * printf.c - Implementation of the formatted output engine.
 *
 * Provides printf(), snprintf(), vprintf(), and vsnprintf() with a common
 * formatting core (fmt_core). All output is buffered locally before a single
 * __libc_write call, eliminating per-character write overhead.
 *
 * Supported specifiers:
 *   %d / %i        signed int
 *   %u             unsigned int
 *   %x / %X        unsigned int hex (lower/upper)
 *   %o             unsigned int octal
 *   %b             unsigned int binary
 *   %ld/%li        signed long
 *   %lu            unsigned long
 *   %lx/%lX        unsigned long hex
 *   %lo            unsigned long octal
 *   %lb            unsigned long binary
 *   %lld/%lli      signed long long
 *   %llu           unsigned long long
 *   %llx/%llX      unsigned long long hex
 *   %zd/%zu/%zx    size_t / ssize_t
 *   %p             pointer (0x-prefixed hex, zero-padded to 16 digits)
 *   %s             string (NULL-safe, prints "(null)")
 *   %c             character
 *   %%             literal percent
 *
 * Width and flags:
 *   %[flags][width][.precision][length]specifier
 *   Flags: '-' (left-align), '0' (zero-pad), '+' (force sign), ' ' (space sign)
 *   Width: decimal integer or '*' (read from va_list)
 *   Precision: decimal integer or '*' for %s (max chars printed)
 *
 * Note on \n handling:
 *   printf() injects \r before every \n to support UART terminals.
 *   snprintf() / vsnprintf() do NOT inject \r — they produce clean strings
 *   suitable for use as message buffers, log entries, etc.
 */

#include "stdio.h"

#include <stdarg.h>
#include "types.h"

#ifdef __KERNEL__
    #include "core/lock.h"
    #include "core/timer.h"
static spinlock_t printf_lock = SPINLOCK_INIT;
#endif

extern void __libc_write(const char* buf, size_t len);

/* Output buffer size for printf's stack buffer.
 * Increase if you routinely format very long lines. */
#define PRINTF_BUF_SIZE 256

/* -------------------------------------------------------------------------
 * Internal formatting context
 * ---------------------------------------------------------------------- */

/*
 * fmt_buf - Output sink abstraction used by fmt_core.
 *
 * For printf/vprintf:  writes directly via __libc_write into a stack buffer
 *                      that is flushed at the end.
 * For snprintf/vsnprintf: writes into the caller-supplied buffer, capping at
 *                      the given size and always NUL-terminating.
 */
struct fmt_buf
{
    char* buf;   /* destination buffer (NULL = use internal flush path) */
    size_t size; /* capacity including the NUL terminator */
    size_t pos;  /* bytes written so far (excluding NUL) */
    int crlf;    /* 1 = translate \n to \r\n, 0 = pass through */
};

/*
 * fb_putc - Appends one character to a fmt_buf.
 * For the printf path (buf == NULL) the character is accumulated in an
 * internal stack buffer; for the snprintf path it goes into buf directly.
 */
static inline void fb_putc(struct fmt_buf* fb, char c)
{
    if (fb->buf)
    {
        /* snprintf path: write into caller buffer up to size-1 bytes */
        if (fb->pos < fb->size - 1)
            fb->buf[fb->pos] = c;
        /* Always increment pos so the caller can detect truncation */
        fb->pos++;
    }
    else
    {
        /* printf path: accumulate in the internal stack buffer.
         * The caller holds a separate stack buffer and flushes at the end;
         * here we call __libc_write directly one char at a time only as a
         * fallback — the real buffering happens in vprintf_impl. */
        __libc_write(&c, 1);
    }
}

/* -------------------------------------------------------------------------
 * Number formatting helpers
 * ---------------------------------------------------------------------- */

/*
 * fmt_uint - Renders an unsigned 64-bit integer into a temporary buffer
 * and returns the number of characters written (without NUL).
 *
 * @val:        value to format
 * @base:       2, 8, 10, or 16
 * @uppercase:  use A-F instead of a-f
 * @out:        caller-supplied buffer of at least 66 bytes (binary needs 64)
 */
static int fmt_uint(uint64_t val, int base, int uppercase, char* out)
{
    if (val == 0)
    {
        out[0] = '0';
        return 1;
    }

    char tmp[66];
    int i = 0;
    while (val)
    {
        unsigned int d = (unsigned int)(val % (unsigned)base);
        if (d < 10)
            tmp[i++] = (char)('0' + d);
        else
            tmp[i++] = (char)((uppercase ? 'A' : 'a') + d - 10);
        val /= (unsigned)base;
    }

    /* Reverse into out */
    for (int j = 0; j < i; j++)
        out[j] = tmp[i - 1 - j];

    return i;
}

/* -------------------------------------------------------------------------
 * Core formatting engine
 * ---------------------------------------------------------------------- */

/*
 * fmt_core - Shared implementation called by all printf variants.
 *
 * Processes the format string and writes results into fb.
 * Returns the total number of characters that would have been written
 * (excluding NUL), even if the buffer was too small (snprintf semantics).
 */
static int fmt_core(struct fmt_buf* fb, const char* fmt, va_list args)
{
    for (const char* p = fmt; *p != '\0'; p++)
    {
        if (*p != '%')
        {
            /* Translate \n to \r\n on the UART printf path */
            if (*p == '\n' && fb->crlf)
                fb_putc(fb, '\r');
            fb_putc(fb, *p);
            continue;
        }

        /* --- Parse format specifier: %[flags][width][.prec][length]spec --- */
        p++;

        /* Flags */
        int flag_left = 0;  /* '-': left-align */
        int flag_zero = 0;  /* '0': zero-pad   */
        int flag_plus = 0;  /* '+': force sign  */
        int flag_space = 0; /* ' ': space sign  */

        while (1)
        {
            if (*p == '-')
            {
                flag_left = 1;
                p++;
            }
            else if (*p == '0')
            {
                flag_zero = 1;
                p++;
            }
            else if (*p == '+')
            {
                flag_plus = 1;
                p++;
            }
            else if (*p == ' ')
            {
                flag_space = 1;
                p++;
            }
            else
                break;
        }
        /* Left-align overrides zero-pad */
        if (flag_left)
            flag_zero = 0;

        /* Width */
        int width = 0;
        if (*p == '*')
        {
            width = va_arg(args, int);
            if (width < 0)
            {
                flag_left = 1;
                flag_zero = 0;
                width = -width;
            }
            p++;
        }
        else
        {
            while (*p >= '0' && *p <= '9')
                width = width * 10 + (*p++ - '0');
        }

        /* Precision */
        int prec = -1; /* -1 = not specified */
        int has_prec = 0;
        if (*p == '.')
        {
            p++;
            has_prec = 1;
            prec = 0;
            if (*p == '*')
            {
                prec = va_arg(args, int);
                if (prec < 0)
                    prec = 0;
                p++;
            }
            else
            {
                while (*p >= '0' && *p <= '9')
                    prec = prec * 10 + (*p++ - '0');
            }
        }
        (void)has_prec;

        /* Length modifier */
        int is_long = 0;     /* 'l'  */
        int is_longlong = 0; /* 'll' */
        int is_size = 0;     /* 'z'  */

        if (*p == 'l')
        {
            p++;
            if (*p == 'l')
            {
                is_longlong = 1;
                p++;
            }
            else
                is_long = 1;
        }
        else if (*p == 'z')
        {
            is_size = 1;
            p++;
        }

        /* --- Specifier --- */
        char num_buf[72]; /* enough for 64-bit binary + prefix + sign */
        int num_len = 0;
        char sign_char = 0;
        int base = 10;
        int uppercase = 0;

        switch (*p)
        {
        /* ---- Signed integers ---- */
        case 'd':
        case 'i':
        {
            int64_t val;
            if (is_longlong)
                val = (int64_t)va_arg(args, long long);
            else if (is_long)
                val = (int64_t)va_arg(args, long);
            else if (is_size)
                val = (int64_t)va_arg(args, ssize_t);
            else
                val = (int64_t)va_arg(args, int);

            uint64_t uval;
            if (val < 0)
            {
                sign_char = '-';
                /* Safe negation of INT64_MIN */
                uval = (uint64_t)(-(val + 1)) + 1ULL;
            }
            else
            {
                if (flag_plus)
                    sign_char = '+';
                else if (flag_space)
                    sign_char = ' ';
                uval = (uint64_t)val;
            }
            num_len = fmt_uint(uval, 10, 0, num_buf);
            goto emit_number;
        }

        /* ---- Unsigned integers ---- */
        case 'u':
            base = 10;
            goto unsigned_common;
        case 'x':
            base = 16;
            uppercase = 0;
            goto unsigned_common;
        case 'X':
            base = 16;
            uppercase = 1;
            goto unsigned_common;
        case 'o':
            base = 8;
            goto unsigned_common;
        case 'b':
            base = 2;
            goto unsigned_common;

unsigned_common:
{
    uint64_t uval;
    if (is_longlong)
        uval = (uint64_t)va_arg(args, unsigned long long);
    else if (is_long)
        uval = (uint64_t)va_arg(args, unsigned long);
    else if (is_size)
        uval = (uint64_t)va_arg(args, size_t);
    else
        uval = (uint64_t)va_arg(args, unsigned int);
    num_len = fmt_uint(uval, base, uppercase, num_buf);
    goto emit_number;
}

emit_number:
{
    /*
     * Padding layout:
     *   [left-pad spaces] [sign] [zero-pad] [digits] [right-pad spaces]
     *
     * Total field width = sign (0 or 1) + num_len
     */
    int field = num_len + (sign_char ? 1 : 0);
    int pad = (width > field) ? width - field : 0;

    if (!flag_left && !flag_zero)
    {
        for (int i = 0; i < pad; i++)
            fb_putc(fb, ' ');
    }
    if (sign_char)
        fb_putc(fb, sign_char);
    if (!flag_left && flag_zero)
    {
        for (int i = 0; i < pad; i++)
            fb_putc(fb, '0');
    }
    for (int i = 0; i < num_len; i++)
        fb_putc(fb, num_buf[i]);
    if (flag_left)
    {
        for (int i = 0; i < pad; i++)
            fb_putc(fb, ' ');
    }
    break;
}

        /* ---- Pointer ---- */
        case 'p':
        {
            unsigned long val = va_arg(args, unsigned long);
            /* Always print as 0x + 16 hex digits (zero-padded) */
            fb_putc(fb, '0');
            fb_putc(fb, 'x');
            char tmp[17];
            int n = fmt_uint((uint64_t)val, 16, 0, tmp);
            /* Zero-pad to exactly 16 digits */
            for (int i = n; i < 16; i++)
                fb_putc(fb, '0');
            for (int i = 0; i < n; i++)
                fb_putc(fb, tmp[i]);
            break;
        }

        /* ---- String ---- */
        case 's':
        {
            const char* s = va_arg(args, const char*);
            if (!s)
                s = "(null)";

            /* Compute printable length, respecting precision if set */
            int slen = 0;
            const char* t = s;
            while (*t)
            {
                if (prec >= 0 && slen >= prec)
                    break;
                slen++;
                t++;
            }

            int pad = (width > slen) ? width - slen : 0;

            if (!flag_left)
                for (int i = 0; i < pad; i++)
                    fb_putc(fb, ' ');

            for (int i = 0; i < slen; i++)
            {
                if (s[i] == '\n' && fb->crlf)
                    fb_putc(fb, '\r');
                fb_putc(fb, s[i]);
            }

            if (flag_left)
                for (int i = 0; i < pad; i++)
                    fb_putc(fb, ' ');

            break;
        }

        /* ---- Character ---- */
        case 'c':
        {
            char c = (char)va_arg(args, int);
            int pad = (width > 1) ? width - 1 : 0;

            if (!flag_left)
                for (int i = 0; i < pad; i++)
                    fb_putc(fb, ' ');

            fb_putc(fb, c);

            if (flag_left)
                for (int i = 0; i < pad; i++)
                    fb_putc(fb, ' ');

            break;
        }

        case '%':
            fb_putc(fb, '%');
            break;

        case '\0':
            /* Trailing lone '%' — back up so the outer loop sees '\0' */
            p--;
            break;

        default:
            /* Unknown specifier: pass through literally */
            fb_putc(fb, '%');
            fb_putc(fb, *p);
            break;
        }
    }

    return (int)fb->pos;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/*
 * vsnprintf - Write at most (size - 1) formatted bytes into buf, always
 * NUL-terminates. Returns the number of bytes that would have been written
 * if size were unlimited (C99 snprintf semantics).
 */
int vsnprintf(char* buf, size_t size, const char* fmt, va_list args)
{
    if (!buf || size == 0)
        return 0;

    struct fmt_buf fb = {
        .buf = buf,
        .size = size,
        .pos = 0,
        .crlf = 0, /* no CRLF translation in string buffers */
    };

    int ret = fmt_core(&fb, fmt, args);

    /* Always NUL-terminate within the provided buffer */
    buf[fb.pos < size ? fb.pos : size - 1] = '\0';
    return ret;
}

/*
 * snprintf - Formatted print into a size-bounded buffer.
 */
int snprintf(char* buf, size_t size, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return ret;
}

/*
 * vprintf - va_list variant of printf.
 *
 * Formats into a stack buffer and flushes in a single __libc_write call
 * to minimise UART contention. If the output exceeds PRINTF_BUF_SIZE,
 * fmt_core will still count correctly but the excess will be silently
 * truncated in the buffer — the PANIC path should use snprintf for
 * arbitrarily long messages.
 */
int vprintf(const char* fmt, va_list args)
{
    char stack_buf[PRINTF_BUF_SIZE];

    struct fmt_buf fb = {
        .buf = stack_buf,
        .size = sizeof(stack_buf),
        .pos = 0,
        .crlf = 1, /* UART: translate \n to \r\n */
    };

    fmt_core(&fb, fmt, args);

    /* NUL-terminate for safety, but write only the formatted bytes */
    size_t write_len = fb.pos < sizeof(stack_buf) ? fb.pos : sizeof(stack_buf) - 1;
    stack_buf[write_len] = '\0';

    if (write_len > 0)
        __libc_write(stack_buf, write_len);

    return (int)fb.pos;
}

/*
 * printf - Formatted console output.
 */
int printf(const char* fmt, ...)
{
#ifdef __KERNEL__
    unsigned long irqflags = spin_lock_irqsave(&printf_lock);
#endif

    va_list args;
    va_start(args, fmt);
    int ret = vprintf(fmt, args);
    va_end(args);

#ifdef __KERNEL__
    spin_unlock_irqrestore(&printf_lock, irqflags);
#endif

    return ret;
}

#ifdef __KERNEL__
/*
 * printk - Kernel logging with system timestamps.
 */
int printk(const char* fmt, ...)
{
    unsigned long irqflags = spin_lock_irqsave(&printf_lock);

    unsigned long ms = get_system_time();
    unsigned long sec = ms / 1000;
    unsigned long rem_ms = ms % 1000;

    /* Prepend the Linux-style timestamp [ seconds.microseconds] */
    char ts_buf[32];
    snprintf(ts_buf, sizeof(ts_buf), "[%5lu.%06lu] ", sec, rem_ms * 1000);

    /* Use vprintf directly to avoid re-locking */
    /* vprintf doesn't have a va_list version for fixed strings easily,
     * so we just write it directly or use snprintf.
     * Actually, we can just call __libc_write if we want to be efficient. */
    size_t ts_len = 0;
    while (ts_buf[ts_len])
        ts_len++;
    __libc_write(ts_buf, ts_len);

    va_list args;
    va_start(args, fmt);
    int ret = vprintf(fmt, args);
    va_end(args);

    spin_unlock_irqrestore(&printf_lock, irqflags);

    return ret;
}
#endif
