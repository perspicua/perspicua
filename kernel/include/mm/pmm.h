/*
 * pmm.h - Public API for the Physical Memory Manager (PMM).
 *
 * This file defines the interface for allocating and freeing physical memory
 * pages, as well as managing reserved physical memory ranges.
 */

#ifndef PERSPICUA_KERNEL_PMM_H
#define PERSPICUA_KERNEL_PMM_H

#include "types.h"

/* Standard physical memory page size (4 KB) */
#define PAGE_SIZE 4096

/*
 * pmm_init - Initializes the physical memory manager. This function
 * discovers available memory from the hardware tree and builds the
 * internal page structures and free lists.
 */
void pmm_init(void);

/*
 * pmm_reserve_range - Marks a specific range of physical memory as reserved.
 * This must be called before pmm_init() to ensure these pages are not
 * added to the buddy allocator's free lists.
 */
void pmm_reserve_range(unsigned long phys_start, unsigned long size, const char* tag);

/*
 * pmm_alloc_page - Allocates a single 4 KB physical memory page.
 * Returns the virtual address of the allocated page, or NULL if out of memory.
 */
void* pmm_alloc_page(void);

/*
 * pmm_free_page - Returns a single physical memory page to the free pool.
 */
void pmm_free_page(void* ptr);

/*
 * pmm_hold_page - Increments the reference count of a physical page,
 * preventing it from being freed until the count returns to zero.
 */
void pmm_hold_page(void* ptr);

/*
 * pmm_alloc_pages - Allocates a contiguous block of physical memory pages.
 * The count must be a power of two if required by the underlying buddy system.
 */
void* pmm_alloc_pages(unsigned long count);

/*
 * pmm_free_pages - Returns a contiguous block of physical pages to the free pool.
 */
void pmm_free_pages(void* ptr, unsigned long count);

/*
 * pmm_get_free_pages - Returns the number of physical pages currently
 * available for allocation.
 */
unsigned long pmm_get_free_pages(void);

/*
 * pmm_get_total_pages - Returns the total number of physical pages
 * managed by the PMM.
 */
unsigned long pmm_get_total_pages(void);

#endif /* PERSPICUA_KERNEL_PMM_H */
