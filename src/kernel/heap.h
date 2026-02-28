#ifndef _HEAP_H_
#define _HEAP_H_

void heap_init(void);
void* kmalloc(unsigned long size);
void kfree(void* ptr);

#endif // _HEAP_H_
