/*
 * fdt.h - Public API for the Flattened Device Tree (FDT) parser.
 *
 * This header defines the structures and functions used to interact with
 * the device tree blob (DTB) provided by the bootloader.
 */

#ifndef PERSPICUA_DEVICETREE_FDT_H
#define PERSPICUA_DEVICETREE_FDT_H

#include "types.h"

/* --- Constants and Macros --- */

#define FDT_MAGIC 0xd00dfeed

#define FDT_BEGIN_NODE 0x00000001
#define FDT_END_NODE   0x00000002
#define FDT_PROP       0x00000003
#define FDT_NOP        0x00000004
#define FDT_END        0x00000009

#define FDT_ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

/* --- Data Structures --- */

/*
 * struct fdt_header - Standard FDT header as it appears in the blob.
 *
 * All fields are Big-Endian. This structure provides offsets to the
 * different blocks of the device tree.
 */
struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

/*
 * struct fdt_reserve_entry - A single memory reservation block.
 */
struct fdt_reserve_entry {
    uint64_t address;
    uint64_t size;
};

/*
 * struct fdt_prop_header - Internal header for a node property.
 */
struct fdt_prop_header {
    uint32_t len;
    uint32_t nameoff;
};

/*
 * struct fdt_property - A parsed property for public consumption.
 */
struct fdt_property {
    const char *name;
    uint32_t size;
    const void *value;
};

/* --- Inline Helper Functions --- */

static inline uint32_t fdt32_to_cpu(uint32_t val)
{
    return ((val & 0x000000ff) << 24) | ((val & 0x0000ff00) << 8) | ((val & 0x00ff0000) >> 8)
           | ((val & 0xff000000) >> 24);
}

static inline uint64_t fdt64_to_cpu(uint64_t val)
{
    return ((uint64_t)fdt32_to_cpu((uint32_t)val) << 32) | fdt32_to_cpu((uint32_t)(val >> 32));
}

/* --- Function Prototypes --- */

/*
 * fdt_init - Initializes the parser with the DTB's base address.
 */
void fdt_init(uintptr_t global_dtb_ptr);

/*
 * fdt_rebase - Updates internal pointers to a new virtual base address.
 */
void fdt_rebase(uintptr_t new_base);

/*
 * fdt_get_property - Retrieves a property by name from a given node.
 */
int fdt_get_property(const uint32_t *node, const char *prop_name, struct fdt_property *out_prop);

/*
 * fdt_find_node_by_path - Locates a node using its absolute path.
 */
const uint32_t *fdt_find_node_by_path(const char *path);

/*
 * fdt_find_node_by_compatible - Locates the first node matching a compatible string.
 */
const uint32_t *fdt_find_node_by_compatible(const char *compatible);

/*
 * fdt_parse_memory_reservations - Informs the PMM of reserved regions.
 */
void fdt_parse_memory_reservations(void);

#endif /* PERSPICUA_DEVICETREE_FDT_H */
