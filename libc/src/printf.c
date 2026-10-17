/*
 * printf.c - Implementation of the formatted output engine.
 */

#include "stdio.h"

#include <stdarg.h>

#include <stddef.h>
#include <stdint.h>

#include "uapi/types.h"

#ifdef __KERNEL__
    #include "arch/cpu.h"
    #include "arch/irq.h"
    #include "core/lock.h"
    #include "core/timer.h"
    #include "panic.h"

// Whole messages go out one at a time, with IRQs left on while one is written.
static spinlock_t printf_lock = SPINLOCK_INIT;
static int printf_owner = -1;

// An IRQ that prints on the core already writing goes straight through rather than wait on itself.
static int printf_lock_take(void)
{
    // A core halted by a panic may hold it for good.
    if (kernel_panicked) {
        return 0;
    }
    for (;;) {
        unsigned long flags = irq_save();
        int core = cpu_id();
        if (__atomic_load_n(&printf_owner, __ATOMIC_RELAXED) == core) {
            irq_restore(flags);
            return 0;
        }
        if (spin_trylock(&printf_lock)) {
            __atomic_store_n(&printf_owner, core, __ATOMIC_RELAXED);
            irq_restore(flags);
            return 1;
        }
        irq_restore(flags);
        while (__atomic_load_n(&printf_lock.locked, __ATOMIC_RELAXED)) {
            asm volatile("yield");
        }
    }
}

static void printf_lock_drop(int taken)
{
    if (taken) {
        unsigned long flags = irq_save();
        __atomic_store_n(&printf_owner, -1, __ATOMIC_RELAXED);
        spin_unlock_irqrestore(&printf_lock, flags);
    }
}
#endif

extern void __libc_write(const char *buf, size_t len);

#define PRINTF_BUF_SIZE 256

// Internal Data Structures

// Output sink abstraction used by fmt_core.
struct fmt_buf {
    char *buf;    // Destination buffer (NULL = write each character out directly)
    size_t size;  // Capacity including the NUL terminator
    size_t pos;   // Bytes currently held in buf
    size_t total; // Characters produced; this is what the printf family returns
    int crlf;     // 1 = translate \n to \r\n, 0 = pass through
    int sink;     // 1 = buf is a scratch sink: flush when full instead of truncating
    int count;    // 1 = only count: snprintf with size 0 sizes its output
};

// Length modifiers, which say what type the argument was passed as.
enum fmt_len {
    LEN_INT,
    LEN_CHAR,
    LEN_SHORT,
    LEN_LONG,
    LEN_LLONG,
    LEN_SIZE,
    LEN_MAX,
    LEN_PTRDIFF,
};

// Private Helper Functions

// Writes out whatever a sink buffer currently holds and empties it.
static inline void fb_flush(struct fmt_buf *fb)
{
    if (fb->pos > 0) {
        __libc_write(fb->buf, fb->pos);
        fb->pos = 0;
    }
}

// Appends one character to a fmt_buf.
static inline void fb_putc(struct fmt_buf *fb, char c)
{
    fb->total++;

    if (fb->count) {
        return;
    }

    if (!fb->buf) {
        __libc_write(&c, 1);
        return;
    }

    if (fb->sink) {
        if (fb->pos == fb->size) {
            fb_flush(fb);
        }
        fb->buf[fb->pos++] = c;
        return;
    }

    if (fb->pos < fb->size - 1) {
        fb->buf[fb->pos] = c;
    }
    fb->pos++;
}

// Renders an unsigned 64-bit integer into a temporary buffer.
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

// Shared implementation called by all printf variants.
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
        int flag_hash = 0;

        for (;;) {
            if (*p == '#') {
                flag_hash = 1;
                p++;
            } else if (*p == '-') {
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

        enum fmt_len len = LEN_INT;
        if (*p == 'h') {
            p++;
            len = LEN_SHORT;
            if (*p == 'h') {
                len = LEN_CHAR;
                p++;
            }
        } else if (*p == 'l') {
            p++;
            len = LEN_LONG;
            if (*p == 'l') {
                len = LEN_LLONG;
                p++;
            }
        } else if (*p == 'z') {
            len = LEN_SIZE;
            p++;
        } else if (*p == 'j') {
            len = LEN_MAX;
            p++;
        } else if (*p == 't') {
            len = LEN_PTRDIFF;
            p++;
        } else if (*p == 'L') {
            // long double: nothing here formats floating point.
            p++;
        }

        char num_buf[72];
        int num_len = 0;
        char sign_char = 0;
        const char *prefix = "";
        int base = 10;
        int uppercase = 0;

        switch (*p) {
            case 'd':
            case 'i': {
                int64_t val;
                switch (len) {
                    case LEN_CHAR:
                        val = (signed char)va_arg(args, int);
                        break;
                    case LEN_SHORT:
                        val = (short)va_arg(args, int);
                        break;
                    case LEN_LONG:
                        val = va_arg(args, long);
                        break;
                    case LEN_LLONG:
                        val = va_arg(args, long long);
                        break;
                    case LEN_SIZE:
                        val = va_arg(args, ssize_t);
                        break;
                    case LEN_MAX:
                        val = va_arg(args, intmax_t);
                        break;
                    case LEN_PTRDIFF:
                        val = va_arg(args, ptrdiff_t);
                        break;
                    default:
                        val = va_arg(args, int);
                        break;
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
                // A precision of 0 prints nothing at all for 0.
                num_len = (prec == 0 && uval == 0) ? 0 : fmt_uint(uval, 10, 0, num_buf);
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
    switch (len) {
        case LEN_CHAR:
            uval = (unsigned char)va_arg(args, unsigned int);
            break;
        case LEN_SHORT:
            uval = (unsigned short)va_arg(args, unsigned int);
            break;
        case LEN_LONG:
            uval = va_arg(args, unsigned long);
            break;
        case LEN_LLONG:
            uval = va_arg(args, unsigned long long);
            break;
        case LEN_SIZE:
            uval = va_arg(args, size_t);
            break;
        case LEN_MAX:
            uval = va_arg(args, uintmax_t);
            break;
        case LEN_PTRDIFF:
            uval = (uint64_t)va_arg(args, ptrdiff_t);
            break;
        default:
            uval = va_arg(args, unsigned int);
            break;
    }
    num_len = (prec == 0 && uval == 0) ? 0 : fmt_uint(uval, base, uppercase, num_buf);

    // '#' marks the base: 0x for a nonzero hex value, and a leading 0 for octal.
    if (flag_hash && base == 16 && uval != 0) {
        prefix = uppercase ? "0X" : "0x";
    } else if (flag_hash && base == 8 && (num_len == 0 || num_buf[0] != '0') && prec <= num_len) {
        prec = num_len + 1;
    }
    goto emit_number;
}

emit_number: {
    // Precision is a minimum digit count, and once given it overrides the '0' flag.
    int zeros = (prec > num_len) ? prec - num_len : 0;
    int prefix_len = prefix[0] ? 2 : 0;
    int field = (sign_char ? 1 : 0) + prefix_len + zeros + num_len;
    int pad = (width > field) ? width - field : 0;
    int zero_pad = !flag_left && flag_zero && prec < 0;

    if (!flag_left && !zero_pad) {
        for (int i = 0; i < pad; i++) {
            fb_putc(fb, ' ');
        }
    }
    if (sign_char) {
        fb_putc(fb, sign_char);
    }
    for (int i = 0; i < prefix_len; i++) {
        fb_putc(fb, prefix[i]);
    }
    if (zero_pad) {
        for (int i = 0; i < pad; i++) {
            fb_putc(fb, '0');
        }
    }
    for (int i = 0; i < zeros; i++) {
        fb_putc(fb, '0');
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

    return (int)fb->total;
}

// Public API Implementations

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    // Nothing to write into: count what would have been written, as the C99 sizing idiom needs.
    if (!buf || size == 0) {
        struct fmt_buf counter = {.count = 1};
        return fmt_core(&counter, fmt, args);
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
        .sink = 1,
/*
 * The kernel writes straight to the UART and must inject CR itself. Userspace
 * output flows through the tty, which already translates \n to \r\n (ONLCR), so
 * it emits plain \n: translating here too would double the CR at the terminal
 * and write \r\n into redirected files and pipes.
 */
#ifdef __KERNEL__
        .crlf = 1,
#else
        .crlf = 0,
#endif
    };

    fmt_core(&fb, fmt, args);
    fb_flush(&fb);

    return (int)fb.total;
}

int printf(const char *fmt, ...)
{
#ifdef __KERNEL__
    int taken = printf_lock_take();
#endif

    va_list args;
    va_start(args, fmt);
    int ret = vprintf(fmt, args);
    va_end(args);

#ifdef __KERNEL__
    printf_lock_drop(taken);
#endif

    return ret;
}

#ifdef __KERNEL__
int printk(const char *fmt, ...)
{
    int taken = printf_lock_take();

    unsigned long ms = timer_get_system_time();
    unsigned long sec = ms / 1000;
    unsigned long rem_ms = ms % 1000;

    char stack_buf[PRINTF_BUF_SIZE];
    struct fmt_buf fb = {
        .buf = stack_buf,
        .size = sizeof(stack_buf),
        .pos = 0,
        .sink = 1,
        .crlf = 1,
    };

    // Shares the body's sink so the line reaches the UART as a single write.
    char ts_buf[32];
    snprintf(ts_buf, sizeof(ts_buf), "[%5lu.%06lu] ", sec, rem_ms * 1000);
    for (const char *t = ts_buf; *t; t++) {
        fb_putc(&fb, *t);
    }
    size_t ts_len = fb.total;

    va_list args;
    va_start(args, fmt);
    fmt_core(&fb, fmt, args);
    va_end(args);

    fb_flush(&fb);

    printf_lock_drop(taken);

    return (int)(fb.total - ts_len);
}
#endif
