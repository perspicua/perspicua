/*
 * heap.h - Public API for the kernel heap allocator.
 */

#ifndef PERSPICUA_MM_HEAP_H
#define PERSPICUA_MM_HEAP_H

void heap_init(void);

void *heap_malloc(unsigned long size);

void heap_free(void *ptr);

unsigned long heap_get_total(void);

#ifdef CONFIG_TESTS
// Bytes the caller may write to an allocation; never less than the size requested.
unsigned long heap_test_usable_size(const void *ptr);

// True while a first-fit block is tagged as handed out.
int heap_test_is_tagged_allocated(const void *ptr);

// Whether the redzone behind an allocation is intact, without heap_free's panic.
int heap_test_redzone_ok(const void *ptr);

// Fails the caller's nth allocation from now (0 disarms); returns what was left, 0 if it fired.
unsigned long heap_test_fail_nth(unsigned long n);
#endif

#endif // PERSPICUA_MM_HEAP_H
