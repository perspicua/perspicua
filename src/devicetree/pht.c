#include "devicetree/pht.h"
#include "lib/string.h"

struct pht_header system_pht = {
    .magic = PHT_MAGIC,
    .nr_devices = 4,
    .nodes = {
        // memory
        {.name = "memory", .address[0] = 0x00000000, .size[0] = 1024 * 1024 * 1024 * 2LU, .reg_count = 1, .irq = 0},
        // uart
        {.name = "uart", .address[0] = 0xFE201000, .size[0] = 0x1000, .reg_count = 1, .irq = 153},
        // general interrupt controller (gic)
        {.name = "gic",
         .address[0] = 0xFF841000,
         .size[0] = 0x1000,
         .address[1] = 0xFF842000,
         .size[1] = 0x2000,
         .reg_count = 2,
         .irq = 0},
        // gpio
        {.name = "gpio", .address[0] = 0xFE200000, .size[0] = 0x1000, .reg_count = 1, .irq = 0}}};

struct pht_node* pht_find_device(const char* name)
{
    if (system_pht.magic != PHT_MAGIC)
        return NULL;

    for (size_t i = 0; i < system_pht.nr_devices; i++)
    {
        if (strcmp(name, system_pht.nodes[i].name) == 0)
        {
            return &system_pht.nodes[i];
        }
    }
    return NULL;
}
