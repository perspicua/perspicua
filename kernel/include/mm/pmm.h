/*
 * pmm.h - Physical Memory Manager public interface.
 *
 * Implements a buddy allocator for physical RAM with reference counting
 * and zero-on-alloc/free guarantees.
 */

#ifndef PERSPICUA_MM_PMM_H
#define PERSPICUA_MM_PMM_H

#include "types.h"
#include "panic.h"

#define PMM_MAX_ORDER           10
#define PAGE_SIZE               4096
#define PMM_MAX_RESERVED_RANGES 16

/* Mark a physical address range as reserved. Must be called before pmm_init. */
void pmm_reserve_range(unsigned long phys_start, unsigned long size, const char* tag);

/* Initialise the buddy allocator. */
void pmm_init(void);

/* Allocate and zero a single 4 KB physical page. */
void* pmm_alloc_page(void);

/* Allocate and zero a power-of-two block of pages. */
void* pmm_alloc_pages(unsigned long count);

/* Decrement reference count; free if zero. */
void pmm_free_page(void* ptr);

/* Free a multi-page block. */
void pmm_free_pages(void* ptr, unsigned long count);

/* Increment the reference count of an allocated page. */
void pmm_hold_page(void* ptr);

/* Check if a virtual address falls in managed RAM. */
int pmm_is_managed(void* ptr);

/* Get approximate number of free pages. */
unsigned long pmm_get_free_pages(void);

/* Get total number of managed pages. */
unsigned long pmm_get_total_pages(void);

#endif /* PERSPICUA_MM_PMM_H */
