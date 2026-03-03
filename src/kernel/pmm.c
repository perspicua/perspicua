#include "pmm.h"
#include "timer.h"
#include "../lib/stdio.h"

extern char __kernel_end[];

#define PHYSICAL_MEMORY_SIZE (1024 * 1024 * 1024) // 1 GB
#define NUM_PAGES (PHYSICAL_MEMORY_SIZE / PAGE_SIZE)
#define BITMAP_SIZE (NUM_PAGES / 8)

static unsigned char* bitmap;

static unsigned long memory_start;
static unsigned long reserved_page_count;
static unsigned long last_alloc_hint;

void pmm_init(void)
{
    bitmap = (unsigned char*)__kernel_end;

    for (unsigned long i = 0; i < BITMAP_SIZE; i++)
        bitmap[i] = 0;

    unsigned long bitmap_end = (unsigned long)bitmap + BITMAP_SIZE;
    memory_start = (bitmap_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    // mark kernel code and bitmap as used
    reserved_page_count = memory_start / PAGE_SIZE;
    for (unsigned long i = 0; i < reserved_page_count; i++)
    {
        unsigned long byte_index = i / 8;
        unsigned long bit_index = i % 8;
        bitmap[byte_index] |= (1 << bit_index);
    }

    last_alloc_hint = reserved_page_count / 8;

    printf("PMM: Initialized. Managing 1GB of RAM.\n");
    printf("PMM: Kernel ends at   : 0x%x\n", (unsigned long)__kernel_end);
    printf("PMM: Bitmap size      : %d bytes\n", (int)BITMAP_SIZE);
    printf("PMM: Usable RAM starts: 0x%x\n", memory_start);
}

void* pmm_alloc_page(void)
{
    unsigned long flags = irq_save();

    // scan by byte from hint, skip fully-used bytes (0xFF)
    unsigned long start_byte = last_alloc_hint;
    for (unsigned long n = 0; n < BITMAP_SIZE; n++)
    {
        unsigned long byte_index = (start_byte + n) % BITMAP_SIZE;

        if (bitmap[byte_index] == 0xFF)
            continue;

        for (unsigned long bit = 0; bit < 8; bit++)
        {
            if ((bitmap[byte_index] & (1 << bit)) == 0)
            {
                bitmap[byte_index] |= (1 << bit);
                last_alloc_hint = byte_index;
                irq_restore(flags);
                return (void*)((byte_index * 8 + bit) * PAGE_SIZE);
            }
        }
    }

    irq_restore(flags);
    printf("PMM: FATAL ERROR - Out of Memory!\n");
    return 0;
}

void pmm_free_page(void* ptr)
{
    unsigned long addr = (unsigned long)ptr;

    if (addr % PAGE_SIZE != 0)
    {
        printf("PMM: WARNING - Tried to free unaligned address 0x%x\n", addr);
        return;
    }

    unsigned long page_index = addr / PAGE_SIZE;

    if (page_index >= NUM_PAGES)
    {
        printf("PMM: WARNING - Tried to free out-of-bounds page 0x%x\n", addr);
        return;
    }

    if (page_index < reserved_page_count)
    {
        printf("PMM: WARNING - Tried to free reserved page 0x%x\n", addr);
        return;
    }

    unsigned long flags = irq_save();

    unsigned long byte_index = page_index / 8;
    unsigned long bit_index = page_index % 8;

    if ((bitmap[byte_index] & (1 << bit_index)) == 0)
    {
        irq_restore(flags);
        printf("PMM: WARNING - Double free detected at 0x%x\n", addr);
        return;
    }

    bitmap[byte_index] &= ~(1 << bit_index);

    irq_restore(flags);
}
