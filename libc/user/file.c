/*
 * file.c - Standard I/O FILE streams implementation.
 */

#include <stddef.h>

#include "uapi/types.h"

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/stat.h"
#include "syscall.h"

// stdout holds data back when it is a pipe or a file; a terminal gets every write at once.
#define STDOUT_BUF 4096

static FILE _stdin = {.fd = 0, .error = 0, .eof = 0};
static FILE _stdout = {.fd = 1, .error = 0, .eof = 0};
static FILE _stderr = {.fd = 2, .error = 0, .eof = 0};

FILE *stdin = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

static char stdout_buf[STDOUT_BUF];
static size_t stdout_len;
static int stdout_mode = -1; // decided at the first write: 1 buffered, 0 not

static int write_all(int fd, const char *s, size_t n)
{
    while (n > 0) {
        int w = write(fd, s, n);
        if (w <= 0) {
            return -1;
        }
        s += w;
        n -= (size_t)w;
    }
    return 0;
}

static int stdout_buffered(void)
{
    if (stdout_mode < 0) {
        struct stat st;
        stdout_mode = fstat(1, &st) == 0 && !S_ISCHR(st.st_mode);
    }
    return stdout_mode;
}

static int stdout_put(const char *s, size_t n)
{
    if (!stdout_buffered()) {
        return write_all(1, s, n);
    }
    if (stdout_len + n > STDOUT_BUF && fflush(stdout) != 0) {
        return -1;
    }
    if (n >= STDOUT_BUF) {
        return write_all(1, s, n);
    }
    memcpy(stdout_buf + stdout_len, s, n);
    stdout_len += n;
    return 0;
}

FILE *fopen(const char *pathname, const char *mode)
{
    int flags = 0;

    if (!pathname || !mode) {
        return NULL;
    }

    if (strchr(mode, 'r')) {
        flags = O_RDONLY;
        if (strchr(mode, '+')) {
            flags = O_RDWR;
        }
    } else if (strchr(mode, 'w')) {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        if (strchr(mode, '+')) {
            flags = O_RDWR | O_CREAT | O_TRUNC;
        }
    } else if (strchr(mode, 'a')) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
        if (strchr(mode, '+')) {
            flags = O_RDWR | O_CREAT | O_APPEND;
        }
    } else {
        return NULL;
    }

    int fd = open(pathname, flags);
    if (fd < 0) {
        return NULL;
    }

    FILE *stream = malloc(sizeof(FILE));
    if (!stream) {
        close(fd);
        return NULL;
    }

    stream->fd = fd;
    stream->error = 0;
    stream->eof = 0;

    return stream;
}

int fclose(FILE *stream)
{
    if (!stream) {
        return EOF;
    }

    if (stream == stdin || stream == stdout || stream == stderr) {
        return 0;
    }

    int res = close(stream->fd);
    free(stream);

    return res < 0 ? EOF : 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (!ptr || !stream || size == 0 || nmemb == 0) {
        return 0;
    }

    int bytes_read = read(stream->fd, ptr, size * nmemb);
    if (bytes_read < 0) {
        stream->error = 1;
        return 0;
    } else if (bytes_read == 0) {
        stream->eof = 1;
        return 0;
    }

    return (size_t)bytes_read / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (!ptr || !stream || size == 0 || nmemb == 0) {
        return 0;
    }

    if (stream == stdout) {
        if (stdout_put(ptr, size * nmemb) != 0) {
            stream->error = 1;
            return 0;
        }
        return nmemb;
    }

    int bytes_written = write(stream->fd, ptr, size * nmemb);
    if (bytes_written < 0) {
        stream->error = 1;
        return 0;
    }

    return (size_t)bytes_written / size;
}

int fseek(FILE *stream, off_t offset, int whence)
{
    if (!stream) {
        return -1;
    }

    int vfs_whence = SEEK_SET;
    if (whence == SEEK_CUR) {
        vfs_whence = SEEK_CUR;
    } else if (whence == SEEK_END) {
        vfs_whence = SEEK_END;
    }

    off_t res = lseek(stream->fd, offset, vfs_whence);
    if (res < 0) {
        stream->error = 1;
        return -1;
    }

    stream->eof = 0;
    return 0;
}

off_t ftell(FILE *stream)
{
    if (!stream) {
        return -1;
    }

    return lseek(stream->fd, 0, SEEK_CUR);
}

int fgetc(FILE *stream)
{
    unsigned char c;
    if (fread(&c, 1, 1, stream) == 1) {
        return c;
    }
    return EOF;
}

int fputc(int c, FILE *stream)
{
    unsigned char uc = (unsigned char)c;
    if (fwrite(&uc, 1, 1, stream) == 1) {
        return uc;
    }
    return EOF;
}

int feof(FILE *stream)
{
    return stream ? stream->eof : 1;
}

int ferror(FILE *stream)
{
    return stream ? stream->error : 1;
}

int fflush(FILE *stream)
{
    // Only stdout ever holds data back, and NULL means every stream.
    if ((stream == stdout || stream == NULL) && stdout_len > 0) {
        size_t n = stdout_len;
        stdout_len = 0;
        if (write_all(1, stdout_buf, n) != 0) {
            stdout->error = 1;
            return EOF;
        }
    }
    return 0;
}

int vfprintf(FILE *stream, const char *fmt, va_list args)
{
    char small[256];
    va_list again;
    va_copy(again, args);
    int len = vsnprintf(small, sizeof(small), fmt, args);

    char *text = small;
    // Too long for the stack buffer: format it again into one that fits.
    if (len >= (int)sizeof(small)) {
        text = malloc((size_t)len + 1);
        if (text) {
            vsnprintf(text, (size_t)len + 1, fmt, again);
        }
    }
    va_end(again);

    if (len < 0 || !text) {
        stream->error = 1;
        return -1;
    }
    size_t written = len > 0 ? fwrite(text, 1, (size_t)len, stream) : 0;
    if (text != small) {
        free(text);
    }
    return written == (size_t)len ? len : -1;
}

int fprintf(FILE *stream, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int ret = vfprintf(stream, fmt, args);
    va_end(args);
    return ret;
}
