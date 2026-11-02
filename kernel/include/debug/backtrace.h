/*
 * backtrace.h - Bounded stack backtrace walker for panic and kdb.
 */

#ifndef PERSPICUA_DEBUG_BACKTRACE_H
#define PERSPICUA_DEBUG_BACKTRACE_H

typedef void (*backtrace_printf_fn)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

int backtrace_frame_ok(unsigned long fp);

void backtrace_print(unsigned long fp, int max_lines, backtrace_printf_fn print_fn);

#endif // PERSPICUA_DEBUG_BACKTRACE_H
