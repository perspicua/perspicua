#ifndef _PMM_H_
#define _PMM_H_

#define PAGE_SIZE 4096

void pmm_init(void);
// Reserve a physical range before pmm_init() builds buddy free lists.
void pmm_reserve_range(unsigned long phys_start, unsigned long size, const char* tag);
void* pmm_alloc_page(void);
void pmm_free_page(void* ptr);
void pmm_hold_page(void* ptr);

// contiguous multi-page allocation
void* pmm_alloc_pages(unsigned long count);
void pmm_free_pages(void* ptr, unsigned long count);

unsigned long pmm_get_free_pages(void);
unsigned long pmm_get_total_pages(void);

#endif
