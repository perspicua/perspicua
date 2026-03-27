/*
 * pmm.h - Public API for the Physical Memory Manager.
 *
 * This header defines the interface for the buddy allocator, providing
 * page-level allocation, reference counting, and reservation management.
 */

#ifndef PERSPICUA_MM_PMM_H
#define PERSPICUA_MM_PMM_H

#include "types.h"

/* --- Constants and Macros --- */

#define PMM_MAX_ORDER 10
#define PAGE_SIZE     4096

/* --- Function Prototypes --- */

/*
 * pmm_init - Discovers RAM from the DTB and initializes the buddy system.
 */
void pmm_init(void);

/*
 * pmm_reserve_range - Marks a physical region as unavailable for allocation.
 */
void pmm_reserve_range(unsigned long phys_start, unsigned long size, const char *tag);

/*
 * pmm_alloc_page - Allocates and zeroes a single 4 KB page.
 */
void *pmm_alloc_page(void);

/*
 * pmm_alloc_pages - Allocates a power-of-two block of pages.
 */
void *pmm_alloc_pages(unsigned long count);

/*
 * pmm_free_page - Decrements refcount; returns to pool if zero.
 */
void pmm_free_page(void *ptr);

/*
 * pmm_free_pages - Frees a block of pages.
 */
void pmm_free_pages(void *ptr, unsigned long count);

/*
 * pmm_hold_page - Increments the reference count of a page.
 */
void pmm_hold_page(void *ptr);

/*
 * pmm_is_managed - Validates if a pointer belongs to the managed RAM pool.
 */
int pmm_is_managed(void *ptr);

/*
 * pmm_get_free_pages - Returns the count of available 4 KB pages.
 */
unsigned long pmm_get_free_pages(void);

/*
 * pmm_get_total_pages - Returns the total managed page count.
 */
unsigned long pmm_get_total_pages(void);

#endif /* PERSPICUA_MM_PMM_H */
