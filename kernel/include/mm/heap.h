/*
 * heap.h - Public API for the kernel heap allocator.
 */

#ifndef PERSPICUA_MM_HEAP_H
#define PERSPICUA_MM_HEAP_H

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

/*
 * True while the redzone behind an allocation is intact. heap_free panics on a
 * corrupt one, which a test can only survive, not assert on; this reports the
 * same condition and returns.
 */
int heap_test_redzone_ok(const void *ptr);

/*
 * Arms a one-shot refusal: the calling task's nth allocation from now returns
 * NULL as though the heap were exhausted. n == 0 disarms. Sizes the allocator
 * would have refused anyway are not counted.
 */
void heap_test_fail_nth(unsigned long n);
#endif

#endif // PERSPICUA_MM_HEAP_H
