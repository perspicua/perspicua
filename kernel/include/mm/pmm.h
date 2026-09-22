/*
 * pmm.h - Public API for the Physical Memory Manager.
 */

#ifndef PERSPICUA_MM_PMM_H
#define PERSPICUA_MM_PMM_H

#define PMM_MAX_ORDER 10
#define PAGE_SIZE     4096

void pmm_init(void);

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
 * Arms a one-shot refusal: the nth page allocation from now returns NULL as
 * though the machine were out of memory. n == 0 disarms.
 */
void pmm_test_fail_nth(unsigned long n);
#endif

#endif // PERSPICUA_MM_PMM_H
