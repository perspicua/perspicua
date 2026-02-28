#ifndef _PMM_H_
#define _PMM_H_

#define PAGE_SIZE 4096

void pmm_init(void);
void* pmm_alloc_page(void);
void pmm_free_page(void* ptr);

#endif
