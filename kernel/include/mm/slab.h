/*
 * slab.h - Public API for the size-class slab allocator.
 *
 * This header defines the interface for a fast, fixed-size object allocator
 * that provides O(1) allocation and deallocation for small kernel objects.
 */

#ifndef PERSPICUA_MM_SLAB_H
#define PERSPICUA_MM_SLAB_H

#include "types.h"

/*
 * slab_init - Seeds the allocator with initial pages for all size classes.
 */
void slab_init(void);

/*
 * slab_alloc - Allocates an object from the best-fit size class.
 */
void *slab_alloc(unsigned long size);

/*
 * slab_free - Returns an object to its parent slab page.
 */
void slab_free(void *ptr);

/*
 * slab_owns - Checks if a pointer was managed by this allocator.
 */
int slab_owns(void *ptr);

/*
 * slab_get_used - Returns the total bytes currently in use by objects.
 */
unsigned long slab_get_used(void);

/*
 * slab_get_total - Returns the total bytes backed by physical pages.
 */
unsigned long slab_get_total(void);

#ifdef CONFIG_TESTS
/*
 * Size class an object was carved from, i.e. the bytes writable in it.
 */
unsigned long slab_test_object_size(void *ptr);
#endif

#endif /* PERSPICUA_MM_SLAB_H */
