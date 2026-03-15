/*
 * heap.h - Public API for the kernel heap allocator.
 *
 * This file defines the interface for dynamic memory allocation in the
 * kernel, including initialization and allocation/deallocation routines.
 */

#ifndef PERSPICUA_KERNEL_HEAP_H
#define PERSPICUA_KERNEL_HEAP_H

#include "types.h"

/*
 * heap_init - Initializes the slab and first-fit heap layers, setting up
 * the initial memory pool.
 */
void heap_init(void);

/*
 * heap_malloc - Allocates a contiguous block of kernel memory of the
 * specified size. Small allocations are handled by the slab layer,
 * while larger ones use a first-fit strategy.
 */
void* heap_malloc(unsigned long size);

/*
 * heap_free - Returns a previously allocated block of memory to the
 * corresponding allocator (slab or first-fit heap).
 */
void heap_free(void* ptr);

/*
 * heap_get_used - Returns the total number of bytes currently allocated
 * across all heap layers.
 */
unsigned long heap_get_used(void);

/*
 * heap_get_total - Returns the total size of the managed heap memory.
 */
unsigned long heap_get_total(void);

#endif /* PERSPICUA_KERNEL_HEAP_H */
