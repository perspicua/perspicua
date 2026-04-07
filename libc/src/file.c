/*
 * file.c - Standard I/O FILE streams implementation.
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "syscall.h"

static FILE _stdin = {.fd = 0, .error = 0, .eof = 0};
static FILE _stdout = {.fd = 1, .error = 0, .eof = 0};
static FILE _stderr = {.fd = 2, .error = 0, .eof = 0};

FILE *stdin = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

FILE *fopen(const char *pathname, const char *mode)
{
    int flags = 0;

    if (!pathname || !mode) {
        return NULL;
    }

    if (strchr(mode, 'r')) {
        flags = VFS_O_RDONLY;
        if (strchr(mode, '+')) {
            flags = VFS_O_RDWR;
        }
    } else if (strchr(mode, 'w')) {
        flags = VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC;
        if (strchr(mode, '+')) {
            flags = VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC;
        }
    } else if (strchr(mode, 'a')) {
        flags = VFS_O_WRONLY | VFS_O_CREAT | VFS_O_APPEND;
        if (strchr(mode, '+')) {
            flags = VFS_O_RDWR | VFS_O_CREAT | VFS_O_APPEND;
        }
    } else {
        return NULL;
    }

    int fd = sys_open(pathname, flags);
    if (fd < 0) {
        return NULL;
    }

    FILE *stream = malloc(sizeof(FILE));
    if (!stream) {
        sys_close(fd);
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

    int res = sys_close(stream->fd);
    free(stream);

    return res < 0 ? EOF : 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (!ptr || !stream || size == 0 || nmemb == 0) {
        return 0;
    }

    int bytes_read = sys_read(stream->fd, ptr, size * nmemb);
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

    int bytes_written = sys_write(stream->fd, ptr, size * nmemb);
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

    int vfs_whence = VFS_SEEK_SET;
    if (whence == SEEK_CUR)
        vfs_whence = VFS_SEEK_CUR;
    else if (whence == SEEK_END)
        vfs_whence = VFS_SEEK_END;

    off_t res = sys_lseek(stream->fd, offset, vfs_whence);
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

    return sys_lseek(stream->fd, 0, VFS_SEEK_CUR);
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
    /* In this simple implementation, writes are unbuffered, so fflush is a no-op.
     * If stream is NULL, we would flush all open streams, but we don't track them.
     * Returns 0 on success, EOF on error.
     */
    (void)stream;
    return 0;
}

int vfprintf(FILE *stream, const char *fmt, va_list args)
{
    char buf[256];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len > 0) {
        size_t write_len = (size_t)len < sizeof(buf) ? (size_t)len : sizeof(buf) - 1;
        fwrite(buf, 1, write_len, stream);
    }
    return len;
}

int fprintf(FILE *stream, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int ret = vfprintf(stream, fmt, args);
    va_end(args);
    return ret;
}
