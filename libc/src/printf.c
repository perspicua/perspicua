/*
 * printf.c - Implementation of the formatted output engine.
 *
 * Provides printf(), snprintf(), vprintf(), and vsnprintf() with a common
 * formatting core. All output is buffered locally to minimize UART overhead.
 */

#include "stdio.h"

#include <stdarg.h>

#include "types.h"

#ifdef __KERNEL__
    #include "core/lock.h"
    #include "core/timer.h"
static spinlock_t printf_lock = SPINLOCK_INIT;
#endif

extern void __libc_write(const char *buf, size_t len);

#define PRINTF_BUF_SIZE 256

/* --- Internal Data Structures --- */

/* Output sink abstraction used by fmt_core. */
struct fmt_buf {
    char *buf;   /* Destination buffer (NULL = use internal flush path) */
    size_t size; /* Capacity including the NUL terminator */
    size_t pos;  /* Bytes written so far (excluding NUL) */
    int crlf;    /* 1 = translate \n to \r\n, 0 = pass through */
};

/* --- Private Helper Functions --- */

/* Appends one character to a fmt_buf. */
static inline void fb_putc(struct fmt_buf *fb, char c)
{
    if (fb->buf) {
        if (fb->pos < fb->size - 1) {
            fb->buf[fb->pos] = c;
        }
        fb->pos++;
    } else {
        __libc_write(&c, 1);
    }
}

/* Renders an unsigned 64-bit integer into a temporary buffer. */
static int fmt_uint(uint64_t val, int base, int uppercase, char *out)
{
    if (val == 0) {
        out[0] = '0';
        return 1;
    }

    char tmp[66];
    int i = 0;
    while (val) {
        unsigned int d = (unsigned int)(val % (unsigned)base);
        if (d < 10) {
            tmp[i++] = (char)('0' + d);
        } else {
            tmp[i++] = (char)((uppercase ? 'A' : 'a') + d - 10);
        }
        val /= (unsigned)base;
    }

    for (int j = 0; j < i; j++) {
        out[j] = tmp[i - 1 - j];
    }

    return i;
}

/* Shared implementation called by all printf variants. */
static int fmt_core(struct fmt_buf *fb, const char *fmt, va_list args)
{
    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            if (*p == '\n' && fb->crlf) {
                fb_putc(fb, '\r');
            }
            fb_putc(fb, *p);
            continue;
        }

        p++;

        int flag_left = 0;
        int flag_zero = 0;
        int flag_plus = 0;
        int flag_space = 0;

        for (;;) {
            if (*p == '-') {
                flag_left = 1;
                p++;
            } else if (*p == '0') {
                flag_zero = 1;
                p++;
            } else if (*p == '+') {
                flag_plus = 1;
                p++;
            } else if (*p == ' ') {
                flag_space = 1;
                p++;
            } else {
                break;
            }
        }

        if (flag_left) {
            flag_zero = 0;
        }

        int width = 0;
        if (*p == '*') {
            width = va_arg(args, int);
            if (width < 0) {
                flag_left = 1;
                flag_zero = 0;
                width = -width;
            }
            p++;
        } else {
            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p++ - '0');
            }
        }

        int prec = -1;
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') {
                prec = va_arg(args, int);
                if (prec < 0) {
                    prec = 0;
                }
                p++;
            } else {
                while (*p >= '0' && *p <= '9') {
                    prec = prec * 10 + (*p++ - '0');
                }
            }
        }

        int is_long = 0;
        int is_longlong = 0;
        int is_size = 0;

        if (*p == 'l') {
            p++;
            if (*p == 'l') {
                is_longlong = 1;
                p++;
            } else {
                is_long = 1;
            }
        } else if (*p == 'z') {
            is_size = 1;
            p++;
        }

        char num_buf[72];
        int num_len = 0;
        char sign_char = 0;
        int base = 10;
        int uppercase = 0;

        switch (*p) {
            case 'd':
            case 'i': {
                int64_t val;
                if (is_longlong) {
                    val = (int64_t)va_arg(args, long long);
                } else if (is_long) {
                    val = (int64_t)va_arg(args, long);
                } else if (is_size) {
                    val = (int64_t)va_arg(args, ssize_t);
                } else {
                    val = (int64_t)va_arg(args, int);
                }

                uint64_t uval;
                if (val < 0) {
                    sign_char = '-';
                    uval = (uint64_t)(-(val + 1)) + 1ULL;
                } else {
                    if (flag_plus) {
                        sign_char = '+';
                    } else if (flag_space) {
                        sign_char = ' ';
                    }
                    uval = (uint64_t)val;
                }
                num_len = fmt_uint(uval, 10, 0, num_buf);
                goto emit_number;
            }

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

unsigned_common: {
    uint64_t uval;
    if (is_longlong) {
        uval = (uint64_t)va_arg(args, unsigned long long);
    } else if (is_long) {
        uval = (uint64_t)va_arg(args, unsigned long);
    } else if (is_size) {
        uval = (uint64_t)va_arg(args, size_t);
    } else {
        uval = (uint64_t)va_arg(args, unsigned int);
    }
    num_len = fmt_uint(uval, base, uppercase, num_buf);
    goto emit_number;
}

emit_number: {
    int field = num_len + (sign_char ? 1 : 0);
    int pad = (width > field) ? width - field : 0;

    if (!flag_left && !flag_zero) {
        for (int i = 0; i < pad; i++) {
            fb_putc(fb, ' ');
        }
    }
    if (sign_char) {
        fb_putc(fb, sign_char);
    }
    if (!flag_left && flag_zero) {
        for (int i = 0; i < pad; i++) {
            fb_putc(fb, '0');
        }
    }
    for (int i = 0; i < num_len; i++) {
        fb_putc(fb, num_buf[i]);
    }
    if (flag_left) {
        for (int i = 0; i < pad; i++) {
            fb_putc(fb, ' ');
        }
    }
    break;
}

            case 'p': {
                unsigned long val = va_arg(args, unsigned long);
                fb_putc(fb, '0');
                fb_putc(fb, 'x');
                char tmp[17];
                int n = fmt_uint((uint64_t)val, 16, 0, tmp);
                for (int i = n; i < 16; i++) {
                    fb_putc(fb, '0');
                }
                for (int i = 0; i < n; i++) {
                    fb_putc(fb, tmp[i]);
                }
                break;
            }

            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) {
                    s = "(null)";
                }

                int slen = 0;
                const char *t = s;
                while (*t) {
                    if (prec >= 0 && slen >= prec) {
                        break;
                    }
                    slen++;
                    t++;
                }

                int pad = (width > slen) ? width - slen : 0;

                if (!flag_left) {
                    for (int i = 0; i < pad; i++) {
                        fb_putc(fb, ' ');
                    }
                }

                for (int i = 0; i < slen; i++) {
                    if (s[i] == '\n' && fb->crlf) {
                        fb_putc(fb, '\r');
                    }
                    fb_putc(fb, s[i]);
                }

                if (flag_left) {
                    for (int i = 0; i < pad; i++) {
                        fb_putc(fb, ' ');
                    }
                }
                break;
            }

            case 'c': {
                char c = (char)va_arg(args, int);
                int pad = (width > 1) ? width - 1 : 0;

                if (!flag_left) {
                    for (int i = 0; i < pad; i++) {
                        fb_putc(fb, ' ');
                    }
                }

                fb_putc(fb, c);

                if (flag_left) {
                    for (int i = 0; i < pad; i++) {
                        fb_putc(fb, ' ');
                    }
                }
                break;
            }

            case '%':
                fb_putc(fb, '%');
                break;

            case '\0':
                p--;
                break;

            default:
                fb_putc(fb, '%');
                fb_putc(fb, *p);
                break;
        }
    }

    return (int)fb->pos;
}

/* --- Public API Implementations --- */

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    if (!buf || size == 0) {
        return 0;
    }

    struct fmt_buf fb = {
        .buf = buf,
        .size = size,
        .pos = 0,
        .crlf = 0,
    };

    int ret = fmt_core(&fb, fmt, args);

    buf[fb.pos < size ? fb.pos : size - 1] = '\0';
    return ret;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return ret;
}

int vprintf(const char *fmt, va_list args)
{
    char stack_buf[PRINTF_BUF_SIZE];

    struct fmt_buf fb = {
        .buf = stack_buf,
        .size = sizeof(stack_buf),
        .pos = 0,
        .crlf = 1,
    };

    fmt_core(&fb, fmt, args);

    size_t write_len = fb.pos < sizeof(stack_buf) ? fb.pos : sizeof(stack_buf) - 1;
    stack_buf[write_len] = '\0';

    if (write_len > 0) {
        __libc_write(stack_buf, write_len);
    }

    return (int)fb.pos;
}

int printf(const char *fmt, ...)
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
int printk(const char *fmt, ...)
{
    unsigned long irqflags = spin_lock_irqsave(&printf_lock);

    unsigned long ms = get_system_time();
    unsigned long sec = ms / 1000;
    unsigned long rem_ms = ms % 1000;

    char ts_buf[32];
    snprintf(ts_buf, sizeof(ts_buf), "[%5lu.%06lu] ", sec, rem_ms * 1000);

    size_t ts_len = 0;
    while (ts_buf[ts_len]) {
        ts_len++;
    }
    __libc_write(ts_buf, ts_len);

    va_list args;
    va_start(args, fmt);
    int ret = vprintf(fmt, args);
    va_end(args);

    spin_unlock_irqrestore(&printf_lock, irqflags);

    return ret;
}
#endif
