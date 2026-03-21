/*
 * fdt.c - Implementation of the Flattened Device Tree (FDT) parser.
 *
 * This file provides the logic for traversing and querying the device
 * tree blob (DTB) provided by the bootloader. It handles token parsing,
 * property retrieval, and memory reservation map processing.
 */

#include "devicetree/fdt.h"

#include "panic.h"
#include "mm/addr.h"
#include "stdio.h"

#include "string.h"

static const uint32_t* fdt_struct_block;
static const char* fdt_strings_block;
static uintptr_t fdt_base_address;

/* Helper to get the next 32-bit token and advance the pointer. */
static inline uint32_t fdt_next_tag(const uint32_t** p)
{
    uint32_t tag = fdt32_to_cpu(**p);
    (*p)++;
    return tag;
}

/* Internal routine to update pointers when the DTB is moved or re-mapped. */
static void fdt_update_pointers(uintptr_t base)
{
    struct fdt_header* fdt = (struct fdt_header*)base;
    if (fdt32_to_cpu(fdt->magic) != FDT_MAGIC)
    {
        PANIC("Invalid FDT: magic number mismatch");
    }

    fdt_base_address = base;
    fdt_struct_block = (const uint32_t*)(base + fdt32_to_cpu(fdt->off_dt_struct));
    fdt_strings_block = (const char*)(base + fdt32_to_cpu(fdt->off_dt_strings));
}

/*
 * Public API Implementations
 */

void fdt_init(uintptr_t global_dtb_ptr)
{
    printf("[  DTB ] Initializing Flattened Device Tree parser...\n");
    fdt_update_pointers(global_dtb_ptr);
    printf("[  DTB ] DTB parsed successfully at 0x%lx\n", global_dtb_ptr);
}

void fdt_rebase(uintptr_t new_base)
{
    fdt_update_pointers(new_base);
    printf("[  DTB ] DTB rebased to virtual address 0x%lx\n", new_base);
}

void fdt_parse_memory_reservations(void)
{
    struct fdt_header* fdt = (struct fdt_header*)fdt_base_address;
    uint32_t totalsize = fdt32_to_cpu(fdt->totalsize);

    extern void pmm_reserve_range(unsigned long phys_start, unsigned long size, const char* tag);

    // Always reserve the DTB itself to prevent overwriting.
    pmm_reserve_range((unsigned long)fdt_base_address, totalsize, "dtb");

    uint32_t rsv_offset = fdt32_to_cpu(fdt->off_mem_rsvmap);
    if (rsv_offset >= totalsize)
    {
        printf("[  DTB ] WARNING: rsvmap offset out of bounds!\n");
        return;
    }

    const struct fdt_reserve_entry* rsvmap = (const struct fdt_reserve_entry*)(fdt_base_address + rsv_offset);
    int count = 0;

    // Parse entries until we hit the null terminator (both address and size are 0).
    while (rsvmap->address != 0 || rsvmap->size != 0)
    {
        uint64_t addr = fdt64_to_cpu(rsvmap->address);
        uint64_t size = fdt64_to_cpu(rsvmap->size);

        if (size > 0)
        {
            printf("[  DTB ] rsvmap[%d]: base=0x%lx size=0x%lx\n", count++, addr, size);
            pmm_reserve_range((unsigned long)addr, (unsigned long)size, "dtb-reserved");
        }
        rsvmap++;

        // Safety limit to prevent infinite loops in malformed blobs.
        if (count > 64)
            break;
    }
}

int fdt_get_property(const uint32_t* node, const char* prop_name, struct fdt_property* out_prop)
{
    if (fdt32_to_cpu(*node) != FDT_BEGIN_NODE)
        return -1;

    const uint32_t* p = node + 1;
    const char* name = (const char*)p;
    p = (const uint32_t*)FDT_ALIGN((uintptr_t)(name + strlen(name) + 1), 4);

    uint32_t tag;
    int depth = 0;
    while ((tag = fdt_next_tag(&p)) != FDT_END)
    {
        if (tag == FDT_BEGIN_NODE)
        {
            depth++;
            const char* n = (const char*)p;
            p = (const uint32_t*)FDT_ALIGN((uintptr_t)(n + strlen(n) + 1), 4);
        }
        else if (tag == FDT_END_NODE)
        {
            if (depth == 0)
                break;
            depth--;
        }
        else if (tag == FDT_PROP)
        {
            const struct fdt_prop_header* hdr = (const struct fdt_prop_header*)p;
            uint32_t len = fdt32_to_cpu(hdr->len);
            const char* cur_name = fdt_strings_block + fdt32_to_cpu(hdr->nameoff);
            const void* value = (const void*)(hdr + 1);
            p = (const uint32_t*)FDT_ALIGN((uintptr_t)value + len, 4);

            if (depth == 0 && strcmp(cur_name, prop_name) == 0)
            {
                if (out_prop)
                {
                    out_prop->name = cur_name;
                    out_prop->size = len;
                    out_prop->value = value;
                }
                return 0;
            }
        }
    }
    return -1;
}

const uint32_t* fdt_find_node_by_compatible(const char* compatible)
{
    const uint32_t* p = fdt_struct_block;
    uint32_t tag;
    const uint32_t* current_node = NULL;

    while ((tag = fdt_next_tag(&p)) != FDT_END)
    {
        if (tag == FDT_BEGIN_NODE)
        {
            current_node = p - 1;
            const char* name = (const char*)p;
            p = (const uint32_t*)FDT_ALIGN((uintptr_t)(name + strlen(name) + 1), 4);
        }
        else if (tag == FDT_PROP)
        {
            const struct fdt_prop_header* hdr = (const struct fdt_prop_header*)p;
            uint32_t len = fdt32_to_cpu(hdr->len);
            const char* cur_name = fdt_strings_block + fdt32_to_cpu(hdr->nameoff);
            const char* value = (const char*)(hdr + 1);
            p = (const uint32_t*)FDT_ALIGN((uintptr_t)value + len, 4);

            if (strcmp(cur_name, "compatible") == 0)
            {
                const char* s = value;
                // compatible is a list of null-terminated strings.
                for (size_t read = 0; read < len; read += strlen(s) + 1, s += strlen(s) + 1)
                {
                    if (strcmp(s, compatible) == 0)
                        return current_node;
                }
            }
        }
    }
    return NULL;
}

const uint32_t* fdt_find_node_by_path(const char* path)
{
    if (!path || path[0] != '/')
        return NULL;

    const uint32_t* p = fdt_struct_block;
    uint32_t tag;

    // Handle root path specially
    if (path[1] == '\0')
        return p;

    const char* current_comp = path + 1;
    int current_depth = 0;
    int search_depth = 1;

    while ((tag = fdt_next_tag(&p)) != FDT_END)
    {
        if (tag == FDT_BEGIN_NODE)
        {
            const uint32_t* node_start = p - 1;
            const char* node_name = (const char*)p;
            p = (const uint32_t*)FDT_ALIGN((uintptr_t)(node_name + strlen(node_name) + 1), 4);

            current_depth++;

            if (current_depth == 1)
            {
                if (node_name[0] == '\0')
                {
                    search_depth = 2;
                    continue;
                }
                else
                {
                    return NULL;
                }
            }

            if (current_depth == search_depth)
            {
                const char* next_slash = strchr(current_comp, '/');
                size_t comp_len = next_slash ? (size_t)(next_slash - current_comp) : strlen(current_comp);

                size_t node_name_len = 0;
                while (node_name[node_name_len] != '\0' && node_name[node_name_len] != '@')
                    node_name_len++;

                size_t comp_name_len = 0;
                while (current_comp[comp_name_len] != '\0' && current_comp[comp_name_len] != '@'
                       && current_comp[comp_name_len] != '/')
                    comp_name_len++;

                int match = 0;
                if (comp_name_len == node_name_len && strncmp(current_comp, node_name, comp_name_len) == 0)
                {
                    const char* comp_unit = strchr(current_comp, '@');
                    if (comp_unit && comp_unit < current_comp + comp_len)
                    {
                        if (strncmp(current_comp, node_name, comp_len) == 0 && node_name[comp_len] == '\0')
                        {
                            match = 1;
                        }
                    }
                    else
                    {
                        match = 1;
                    }
                }

                if (match)
                {
                    if (!next_slash || next_slash[1] == '\0')
                    {
                        return node_start;
                    }
                    current_comp = next_slash + 1;
                    search_depth++;
                    continue;
                }
            }

            // Skip this subtree
            int skip_depth = 1;
            while (skip_depth > 0 && (tag = fdt_next_tag(&p)) != FDT_END)
            {
                if (tag == FDT_BEGIN_NODE)
                {
                    skip_depth++;
                    const char* n = (const char*)p;
                    p = (const uint32_t*)FDT_ALIGN((uintptr_t)(n + strlen(n) + 1), 4);
                }
                else if (tag == FDT_END_NODE)
                    skip_depth--;
                else if (tag == FDT_PROP)
                {
                    const struct fdt_prop_header* hdr = (const struct fdt_prop_header*)p;
                    p = (const uint32_t*)FDT_ALIGN((uintptr_t)(hdr + 1) + fdt32_to_cpu(hdr->len), 4);
                }
            }
            current_depth--;
        }
        else if (tag == FDT_END_NODE)
        {
            current_depth--;
        }
        else if (tag == FDT_PROP)
        {
            const struct fdt_prop_header* hdr = (const struct fdt_prop_header*)p;
            p = (const uint32_t*)FDT_ALIGN((uintptr_t)(hdr + 1) + fdt32_to_cpu(hdr->len), 4);
        }
    }

    return NULL;
}
