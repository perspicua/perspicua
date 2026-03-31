#include "test.h"
#include "mm/heap.h"
#include "mm/slab.h"
#include "stdio.h"
#include "panic.h"

void test_kasan_heap(void)
{
    printk("test: running KASAN heap overflow test (expect panic)\n");
    char *ptr = heap_malloc(64);
    // write past the end to corrupt the redzone footer
    ptr[64] = 0xFF;
    ptr[65] = 0xFF;
    ptr[66] = 0xFF;
    ptr[67] = 0xFF;

    // this should trigger the panic
    heap_free(ptr);
}

void test_kasan_slab(void)
{
    printk("test: running KASAN slab overflow test (expect panic)\n");
    char *ptr = slab_alloc(16);
    // write past the end to corrupt the redzone
    ptr[16] = 0xAA;

    // this should trigger the panic
    slab_free(ptr);
}
