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

/*
 * pmm_parse_ram_ranges - Reads a devicetree /memory reg property into ranges.
 *
 * Keeps what lies below limit, page-aligned and sorted by start; the bytes cut
 * off at limit are added to *ignored. Returns the number of ranges, or a
 * negative errno for a property that does not describe disjoint RAM.
 */
int pmm_parse_ram_ranges(const uint32_t *cells, unsigned long bytes, uint32_t addr_cells,
                         uint32_t size_cells, unsigned long limit, struct pmm_range *out, int max,
                         unsigned long *ignored);

void pmm_init(void);

// The RAM ranges pmm_init found, sorted by start; returns how many.
int pmm_get_ram_ranges(const struct pmm_range **ranges);

void pmm_reserve_range(unsigned long phys_start, unsigned long size, const char *tag);

/*
 * pmm_alloc_page - Allocates and zeroes a single 4 KB page.
 */
void *pmm_alloc_page(void);

/*
 * pmm_alloc_pages - Allocates a power-of-two block of pages, zeroing them.
 */
void *pmm_alloc_pages(unsigned long count);

/*
 * pmm_alloc_pages_nozero - Allocates a power-of-two block of pages, as they
 * came back from their last owner.
 *
 * The caller must overwrite every byte before the memory becomes reachable
 * from userspace; whatever it leaves untouched is the previous owner's data.
 */
void *pmm_alloc_pages_nozero(unsigned long count);

void pmm_free_page(void *ptr);

void pmm_free_pages(void *ptr);

void pmm_hold_page(void *ptr);

void pmm_reserve_range(unsigned long phys_start, unsigned long size, const char *tag);

int pmm_is_managed(void *ptr);

int pmm_is_slab(void *ptr);

void pmm_set_slab(void *ptr, int is_slab);

unsigned int pmm_page_refcount(void *ptr);

unsigned long pmm_get_free_pages(void);

unsigned long pmm_get_total_pages(void);

#ifdef CONFIG_TESTS
/*
 * Arms a one-shot refusal: the calling task's nth page allocation from now
 * returns NULL as though the machine were out of memory. n == 0 disarms.
 * Orders the allocator would have refused anyway are not counted.
 */
void pmm_test_fail_nth(unsigned long n);
#endif

#endif // PERSPICUA_MM_PMM_H
