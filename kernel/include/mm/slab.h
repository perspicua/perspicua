/*
 * slab.h - Public API for the size-class slab allocator.
 */

#ifndef PERSPICUA_MM_SLAB_H
#define PERSPICUA_MM_SLAB_H

#include "types.h"

void slab_init(void);

/*
 * slab_alloc - Allocates an object from the best-fit size class.
 */
void *slab_alloc(unsigned long size);

void slab_free(void *ptr);

int slab_owns(void *ptr);

unsigned long slab_get_used(void);

unsigned long slab_get_total(void);

#ifdef CONFIG_TESTS
/*
 * Size class an object was carved from, i.e. the bytes writable in it.
 */
unsigned long slab_test_object_size(void *ptr);
#endif

#endif // PERSPICUA_MM_SLAB_H
