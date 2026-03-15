/*
 * pht.h - Perspicua Hardware Tree (PHT) public API.
 *
 * This file defines the structures and functions used to interact with the
 * hardware tree, which describes the system's memory and peripheral layout.
 */

#ifndef PERSPICUA_DEVICETREE_PHT_H
#define PERSPICUA_DEVICETREE_PHT_H

#include "types.h"

#define PHT_MAGIC 0x52494349 /* RICI */

/*
 * pht_node - Represents a single hardware device or memory region.
 */
struct pht_node
{
    char name[32];
    uintptr_t address[2];
    size_t size[2];
    uint8_t reg_count;
    uint32_t irq;
} __attribute__((aligned(16)));

/*
 * pht_header - The root structure of the Perspicua Hardware Tree.
 */
struct pht_header
{
    uint32_t magic;
    size_t nr_devices;
    struct pht_node nodes[16];
} __attribute__((aligned(16)));

/*
 * pht_find_device - Searches the hardware tree for a device by its name.
 * Returns a pointer to the node if found, or NULL otherwise.
 */
struct pht_node* pht_find_device(const char* name);

#endif /* PERSPICUA_DEVICETREE_PHT_H */
