/*
 * malloc.c - Userspace heap allocator using mmap.
 */

#include "stdlib.h"
#include "string.h"
#include "syscall.h"
#include "uapi/mman.h"

#define HEAP_ALIGN(x) (((x) + 15) & ~15UL)
#define CHUNK_SIZE    (1024 * 1024)

struct block_header {
    size_t size;
    struct block_header *next;
    int is_free;
} __attribute__((aligned(16)));

#define HEADER_SIZE sizeof(struct block_header)

static struct block_header *block_list = NULL;

static void coalesce()
{
    struct block_header *curr = block_list;
    while (curr && curr->next) {
        if (curr->is_free && curr->next->is_free) {
            if ((char *)curr + HEADER_SIZE + curr->size == (char *)curr->next) {
                curr->size += HEADER_SIZE + curr->next->size;
                curr->next = curr->next->next;
                continue; // Try to coalesce again with the new next
            }
        }
        curr = curr->next;
    }
}

static struct block_header *request_space(size_t size)
{
    size_t total_needed = size + HEADER_SIZE;
    size_t mmap_size = (total_needed + CHUNK_SIZE - 1) & ~(CHUNK_SIZE - 1);

    void *ptr =
        sys_mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        return NULL;
    }

    struct block_header *new_chunk = (struct block_header *)ptr;
    new_chunk->size = mmap_size - HEADER_SIZE;
    new_chunk->is_free = 1;
    new_chunk->next = NULL;

    // Add to block_list in address order
    if (!block_list || new_chunk < block_list) {
        new_chunk->next = block_list;
        block_list = new_chunk;
    } else {
        struct block_header *curr = block_list;
        while (curr->next && curr->next < new_chunk) {
            curr = curr->next;
        }
        new_chunk->next = curr->next;
        curr->next = new_chunk;
    }

    return new_chunk;
}

void *malloc(size_t size)
{
    if (size == 0)
        return NULL;
    size = HEAP_ALIGN(size);

    struct block_header *curr = block_list;
    while (curr) {
        if (curr->is_free && curr->size >= size) {
            if (curr->size >= size + HEADER_SIZE + 16) {
                struct block_header *split =
                    (struct block_header *)((char *)curr + HEADER_SIZE + size);
                split->size = curr->size - size - HEADER_SIZE;
                split->is_free = 1;
                split->next = curr->next;

                curr->size = size;
                curr->next = split;
            }
            curr->is_free = 0;
            return (void *)((char *)curr + HEADER_SIZE);
        }
        curr = curr->next;
    }

    if (!request_space(size))
        return NULL;
    return malloc(size);
}

void free(void *ptr)
{
    if (!ptr)
        return;

    struct block_header *block = (struct block_header *)((char *)ptr - HEADER_SIZE);
    block->is_free = 1;
    coalesce();
}

void *calloc(size_t nmemb, size_t size)
{
    /* Reject nmemb * size overflow before it produces an undersized buffer. */
    if (size != 0 && nmemb > (size_t)-1 / size) {
        return NULL;
    }
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr)
        memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr)
        return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    struct block_header *block = (struct block_header *)((char *)ptr - HEADER_SIZE);
    if (block->size >= size)
        return ptr;

    void *new_ptr = malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, block->size);
        free(ptr);
    }
    return new_ptr;
}

void exit(int status)
{
    sys_exit(status);
}
