#ifndef _PMM_H_
#define _PMM_H_

#define PAGE_SIZE 4096

void pmm_init(void);
void* pmm_alloc_page(void);
void pmm_free_page(void* ptr);
void pmm_hold_page(void* ptr);

// contiguous multi-page allocation
void* pmm_alloc_pages(unsigned long count);
void pmm_free_pages(void* ptr, unsigned long count);

#endif
