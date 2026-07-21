/*
 * malloc.c - Kernel-mode libc allocator glue.
 *
 * The common libc sources allocate through the standard malloc/free API.
 * In kernel mode those calls are routed to the kernel heap instead of the
 * userspace mmap allocator in libc/src/malloc.c.
 */

#include "stdlib.h"

#include "panic.h"
#include "string.h"

#include "mm/heap.h"

void *malloc(size_t size)
{
    return heap_malloc(size);
}

void free(void *ptr)
{
    heap_free(ptr);
}

void *calloc(size_t nmemb, size_t size)
{
    /* Reject nmemb * size overflow before it produces an undersized buffer. */
    if (size != 0 && nmemb > (size_t)-1 / size) {
        return NULL;
    }

    size_t total = nmemb * size;
    void *ptr = heap_malloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void exit(int status)
{
    (void)status;
    PANIC("exit() called in kernel mode");

    for (;;) {
        asm volatile("wfe");
    }
}
