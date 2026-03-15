/*
 * fdt.h - Public API for the Flattened Device Tree (FDT) parser.
 *
 * This header defines the structures and functions used to interact with
 * the device tree blob (DTB) provided by the bootloader.
 */

#ifndef PERSPICUA_DEVICETREE_FDT_H
#define PERSPICUA_DEVICETREE_FDT_H

#include "types.h"

/* FDT Magic Number */
#define FDT_MAGIC 0xd00dfeed

/* FDT Tokens */
#define FDT_BEGIN_NODE 0x00000001
#define FDT_END_NODE   0x00000002
#define FDT_PROP       0x00000003
#define FDT_NOP        0x00000004
#define FDT_END        0x00000009

/* Alignment macro for DTB strings and data. */
#define FDT_ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

/*
 * The FDT header structure as it appears in the blob.
 * All fields are big-endian.
 */
struct fdt_header
{
    uint32_t magic;             /* magic word FDT_MAGIC */
    uint32_t totalsize;         /* total size of DT block */
    uint32_t off_dt_struct;     /* offset to structure */
    uint32_t off_dt_strings;    /* offset to strings */
    uint32_t off_mem_rsvmap;    /* offset to memory reserve map */
    uint32_t version;           /* format version */
    uint32_t last_comp_version; /* last compatible version */
    uint32_t boot_cpuid_phys;   /* physical CPU id */
    uint32_t size_dt_strings;   /* size of the strings block */
    uint32_t size_dt_struct;    /* size of the structure block */
};

/* Memory reservation block entry. */
struct fdt_reserve_entry
{
    uint64_t address;
    uint64_t size;
};

/* Internal FDT property header as it appears in the blob. */
struct fdt_prop_header
{
    uint32_t len;
    uint32_t nameoff;
};

/* Parsed property structure for ease of use in the public API. */
struct fdt_property
{
    const char* name;
    uint32_t size;
    const void* value;
};

/* Endianness swapping utilities (Big-Endian to Little-Endian) */
static inline uint32_t fdt32_to_cpu(uint32_t val)
{
    return ((val & 0x000000ff) << 24) | ((val & 0x0000ff00) << 8) | ((val & 0x00ff0000) >> 8)
           | ((val & 0xff000000) >> 24);
}

static inline uint64_t fdt64_to_cpu(uint64_t val)
{
    return ((uint64_t)fdt32_to_cpu((uint32_t)val) << 32) | fdt32_to_cpu((uint32_t)(val >> 32));
}

/*
 * Initialization and Re-basing
 */

/* Initializes the FDT parser with the physical address of the DTB. */
void fdt_init(uintptr_t global_dtb_ptr);

/* Updates the FDT internal pointers to a new base address (e.g., post-MMU). */
void fdt_rebase(uintptr_t new_base);

/*
 * Query API
 */

/* Retrieves a property by name from a given FDT node. */
int fdt_get_property(const uint32_t* node, const char* prop_name, struct fdt_property* out_prop);

/* Finds a node by its absolute path in the device tree. */
const uint32_t* fdt_find_node_by_path(const char* path);

/* Finds the first node that matches a specific compatible string. */
const uint32_t* fdt_find_node_by_compatible(const char* compatible);

/*
 * Memory Reservation API
 */

/* Parses the memory reservation map and registers ranges with the PMM. */
void fdt_parse_memory_reservations(void);

#endif /* PERSPICUA_DEVICETREE_FDT_H */
