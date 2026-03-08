// perspicua hardware tree

#ifndef _PHT_H_
#define _PHT_H_

#include "lib/types.h"

#define PHT_MAGIC 0x52494349 // RICI because Riciu is cool

struct pht_node
{
    char name[32];
    uintptr_t address[2];
    size_t size[2];
    uint8_t reg_count;
    uint32_t irq;
} __attribute__((aligned(16)));

struct pht_header
{
    uint32_t magic;
    size_t nr_devices;
    struct pht_node nodes[16];
} __attribute__((aligned(16)));

struct pht_node* pht_find_device(const char* name);

#endif // _PHT_H_
