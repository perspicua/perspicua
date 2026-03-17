/*
 * slab.h - Public API for the size-class slab allocator.
 *
 * This file defines the interface for a fast, fixed-size object allocator
 * that minimizes fragmentation and provides O(1) allocation and deallocation.
 */

#ifndef PERSPICUA_KERNEL_SLAB_H
#define PERSPICUA_KERNEL_SLAB_H

#include "types.h"

/*
 * slab_init - Initializes the slab allocator by pre-allocating an initial
 * slab page for each supported size class.
 */
void slab_init(void);

/*
 * slab_alloc - Allocates a contiguous block of memory of at least the
 * specified size from the appropriate size class pool. Returns NULL
 * if the size exceeds the maximum slab size or if memory is exhausted.
 */
void* slab_alloc(unsigned long size);

/*
 * slab_free - Returns an object to its corresponding slab page freelist.
 */
void slab_free(void* ptr);

/*
 * slab_owns - Checks if the given pointer was originally allocated by
 * the slab allocator by verifying the slab magic number at the page boundary.
 */
int slab_owns(void* ptr);

/*
 * slab_get_used - Calculates the total number of bytes currently
 * allocated across all slab classes.
 */
unsigned long slab_get_used(void);

/*
 * slab_get_total - Calculates the total capacity of all currently
 * allocated slab pages.
 */
unsigned long slab_get_total(void);

#endif /* PERSPICUA_KERNEL_SLAB_H */
