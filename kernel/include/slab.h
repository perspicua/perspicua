#ifndef _SLAB_H_
#define _SLAB_H_

void slab_init(void);

void* slab_alloc(unsigned long size);
void slab_free(void* ptr);

// returns 1 if ptr was allocated by the slab allocator, 0 otherwise
int slab_owns(void* ptr);

#endif // _SLAB_H_
