/*
 * pmm.h - Public API for the Physical Memory Manager.
 */

#ifndef PERSPICUA_MM_PMM_H
#define PERSPICUA_MM_PMM_H

#include "types.h"

#define PMM_MAX_ORDER 10
#define PAGE_SIZE     4096

void pmm_init(void);

void pmm_reserve_range(unsigned long phys_start, unsigned long size, const char *tag);

/*
 * pmm_alloc_page - Allocates and zeroes a single 4 KB page.
 */
void *pmm_alloc_page(void);

/*
 * pmm_alloc_pages - Allocates a power-of-two block of pages.
 */
void *pmm_alloc_pages(unsigned long count);

void pmm_free_page(void *ptr);

void pmm_free_pages(void *ptr, unsigned long count);

void pmm_hold_page(void *ptr);

int pmm_is_managed(void *ptr);

unsigned int pmm_page_refcount(void *ptr);

unsigned long pmm_get_free_pages(void);

unsigned long pmm_get_total_pages(void);

#endif // PERSPICUA_MM_PMM_H
