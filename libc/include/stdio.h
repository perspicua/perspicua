/*
 * stdio.h - Kernel/user formatted output API.
 */

#ifndef PERSPICUA_LIBC_STDIO_H
#define PERSPICUA_LIBC_STDIO_H

#include <stddef.h>

#include "uapi/types.h"

#include "uapi/fcntl.h"
#include <stdarg.h>

#define EOF (-1)

int rename(const char *oldpath, const char *newpath);

typedef struct _FILE {
    int fd;
    int error;
    int eof;
    int ungot; // 1 + the byte ungetc pushed back, read before the descriptor; 0 for none
} FILE;

// setvbuf modes. Only stdout holds data back: line buffering there writes at once.
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#define BUFSIZ 4096

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *pathname, const char *mode);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fseek(FILE *stream, off_t offset, int whence);
off_t ftell(FILE *stream);
int fgetc(FILE *stream);
int fputc(int c, FILE *stream);
int getchar(void);
int putchar(int c);
// One byte pushed back, which the next read returns; EOF if one is already waiting.
int ungetc(int c, FILE *stream);
// Reads up to size - 1 bytes, stopping after a newline; NULL at end of file with nothing read.
char *fgets(char *s, int size, FILE *stream);
int fputs(const char *s, FILE *stream);
int puts(const char *s);
int setvbuf(FILE *stream, char *buf, int mode, size_t size);
int feof(FILE *stream);
int ferror(FILE *stream);
int fflush(FILE *stream);
int fprintf(FILE *stream, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vfprintf(FILE *stream, const char *fmt, va_list args);

// Formatted output to stdout; the kernel's goes to the UART.
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __KERNEL__
// Kernel-specific logging that prepends a system timestamp.
int printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

    #define pr_info(fmt, ...) printk(fmt, ##__VA_ARGS__)
    #define pr_err(fmt, ...)  printk("ERROR: " fmt, ##__VA_ARGS__)
    #define pr_warn(fmt, ...) printk("WARNING: " fmt, ##__VA_ARGS__)
    #ifdef CONFIG_DEBUG_LOG
        #define pr_debug(fmt, ...) printk("DEBUG: " fmt, ##__VA_ARGS__)
    #else
        // Still type-checks the format and arguments when compiled out.
        #define pr_debug(fmt, ...)              \
            do {                                \
                if (0) {                        \
                    printk(fmt, ##__VA_ARGS__); \
                }                               \
            } while (0)
    #endif
#endif

int vprintf(const char *fmt, va_list args);

// Writes at most size bytes, terminated when size > 0; returns the length it wanted.
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);

#endif // PERSPICUA_LIBC_STDIO_H
