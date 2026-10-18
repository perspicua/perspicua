/*
 * pmm.h - Public API for the Physical Memory Manager.
 */

#ifndef PERSPICUA_MM_PMM_H
#define PERSPICUA_MM_PMM_H

#include <stdint.h>

#define PMM_MAX_ORDER 10
#define PAGE_SIZE     4096

#define PMM_MAX_RAM_RANGES 8

// A page-aligned span of physical RAM, [start, end).
struct pmm_range {
    unsigned long start;
    unsigned long end;
};

// Ranges below limit, page-aligned and sorted, or -errno if they overlap; the cut adds to *ignored.
int pmm_parse_ram_ranges(const uint32_t *cells, unsigned long bytes, uint32_t addr_cells,
                         uint32_t size_cells, unsigned long limit, struct pmm_range *out, int max,
                         unsigned long *ignored);

void pmm_init(void);

// The RAM ranges pmm_init found, sorted by start; returns how many.
int pmm_get_ram_ranges(const struct pmm_range **ranges);

void pmm_reserve_range(unsigned long phys_start, unsigned long size, const char *tag);

// A zeroed 4 KB page.
void *pmm_alloc_page(void);

// A zeroed power-of-two block of pages.
void *pmm_alloc_pages(unsigned long count);

// Not zeroed: the caller overwrites every byte before userspace can reach any of it.
void *pmm_alloc_pages_nozero(unsigned long count);

void pmm_free_page(void *ptr);

void pmm_free_pages(void *ptr);

void pmm_hold_page(void *ptr);

int pmm_is_managed(void *ptr);

int pmm_is_slab(void *ptr);

void pmm_set_slab(void *ptr, int is_slab);

unsigned int pmm_page_refcount(void *ptr);

unsigned long pmm_get_free_pages(void);

unsigned long pmm_get_total_pages(void);

#ifdef CONFIG_TESTS
// Fails the caller's nth page allocation from now (0 disarms); returns what was left, 0 if fired.
unsigned long pmm_test_fail_nth(unsigned long n);

// The order of the free block that starts at ptr, or -1 if none does.
int pmm_test_free_order(void *ptr);
#endif

#endif // PERSPICUA_MM_PMM_H
