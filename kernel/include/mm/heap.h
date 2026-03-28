/*
 * heap.h - Public API for the kernel heap allocator.
 *
 * This header defines the interface for dynamic memory allocation, combining
 * a fast slab layer for small objects and a first-fit allocator for larger ones.
 */

#ifndef PERSPICUA_MM_HEAP_H
#define PERSPICUA_MM_HEAP_H

#include "types.h"

/*
 * heap_init - Boot-time initialization of heap layers and initial pool.
 */
void heap_init(void);

/*
 * heap_malloc - Allocates a contiguous kernel memory block.
 */
void *heap_malloc(unsigned long size);

/*
 * heap_free - Returns an allocated block to its respective pool.
 */
void heap_free(void *ptr);

/*
 * heap_get_used - Returns the total bytes currently allocated across all layers.
 */
unsigned long heap_get_used(void);

/*
 * heap_get_total - Returns the total size of managed heap memory.
 */
unsigned long heap_get_total(void);

#endif /* PERSPICUA_MM_HEAP_H */
