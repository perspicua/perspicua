/*
 * heap.h - Public API for the kernel heap allocator.
 */

#ifndef PERSPICUA_MM_HEAP_H
#define PERSPICUA_MM_HEAP_H

#include "types.h"

void heap_init(void);

void *heap_malloc(unsigned long size);

void heap_free(void *ptr);

unsigned long heap_get_used(void);

unsigned long heap_get_total(void);

#ifdef CONFIG_TESTS
/*
 * Bytes the caller may write to an allocation. Must never be less than the
 * size requested.
 */
unsigned long heap_test_usable_size(const void *ptr);

// True while a first-fit block is tagged as handed out.
int heap_test_is_tagged_allocated(const void *ptr);
#endif

#endif // PERSPICUA_MM_HEAP_H
