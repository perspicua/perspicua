/*
 * fdt.c - Implementation of the Flattened Device Tree (FDT) parser.
 *
 * This module provides logic for traversing and querying the DTB provided
 * by the bootloader. It handles token parsing, property retrieval, and
 * memory reservation mapping.
 */

#include "devicetree/fdt.h"

#include "stdio.h"
#include "string.h"
#include "panic.h"

#include "mm/addr.h"

static const uint32_t *fdt_struct_block;
static const char *fdt_strings_block;
static uintptr_t fdt_base_address;

/*
 * fdt_next_tag - Extracts the next token and advances the cursor.
 */
static inline uint32_t fdt_next_tag(const uint32_t **p)
{
    uint32_t tag = fdt32_to_cpu(**p);
    (*p)++;
    return tag;
}

/*
 * fdt_update_pointers - Internal routine to update block pointers.
 *
 * Used during initialization and rebasing when the DTB mapping changes.
 */
static void fdt_update_pointers(uintptr_t base)
{
    struct fdt_header *fdt = (struct fdt_header *)base;
    if (fdt32_to_cpu(fdt->magic) != FDT_MAGIC) {
        PANIC("Invalid FDT: magic mismatch");
    }

    fdt_base_address = base;
    fdt_struct_block = (const uint32_t *)(base + fdt32_to_cpu(fdt->off_dt_struct));
    fdt_strings_block = (const char *)(base + fdt32_to_cpu(fdt->off_dt_strings));
}

/*
 * fdt_init - Configures the parser with the initial physical DTB address.
 */
void fdt_init(uintptr_t global_dtb_ptr)
{
    pr_info("dtb: initializing flattened device tree parser...\n");
    fdt_update_pointers(global_dtb_ptr);
}

/*
 * fdt_rebase - Migrates parser pointers to a new virtual address mapping.
 */
void fdt_rebase(uintptr_t new_base)
{
    fdt_update_pointers(new_base);
}

/*
 * fdt_parse_memory_reservations - Informs the memory manager of reserved regions.
 */
void fdt_parse_memory_reservations(void)
{
    struct fdt_header *fdt = (struct fdt_header *)fdt_base_address;
    uint32_t totalsize = fdt32_to_cpu(fdt->totalsize);

    extern void pmm_reserve_range(unsigned long phys_start, unsigned long size, const char *tag);

    /* Protect the DTB itself from being reclaimed */
    pmm_reserve_range((unsigned long)fdt_base_address, totalsize, "dtb");

    uint32_t rsv_offset = fdt32_to_cpu(fdt->off_mem_rsvmap);
    if (rsv_offset >= totalsize) {
        return;
    }

    const struct fdt_reserve_entry *rsvmap =
        (const struct fdt_reserve_entry *)(fdt_base_address + rsv_offset);
    int count = 0;

    /* Parse until the null termination entry */
    while (rsvmap->address != 0 || rsvmap->size != 0) {
        uint64_t addr = fdt64_to_cpu(rsvmap->address);
        uint64_t size = fdt64_to_cpu(rsvmap->size);

        if (size > 0) {
            pr_info("dtb: rsvmap[%d]: base=0x%lx size=0x%lx\n", count++, addr, size);
            pmm_reserve_range((unsigned long)addr, (unsigned long)size, "dtb-reserved");
        }
        rsvmap++;

        if (count > 64) {
            break;
        }
    }
}

/*
 * fdt_get_property - Searches a specific node for a property by name.
 */
int fdt_get_property(const uint32_t *node, const char *prop_name, struct fdt_property *out_prop)
{
    if (fdt32_to_cpu(*node) != FDT_BEGIN_NODE) {
        return -1;
    }

    const uint32_t *p = node + 1;
    const char *name = (const char *)p;
    p = (const uint32_t *)FDT_ALIGN((uintptr_t)(name + strlen(name) + 1), 4);

    uint32_t tag;
    int depth = 0;

    while ((tag = fdt_next_tag(&p)) != FDT_END) {
        if (tag == FDT_BEGIN_NODE) {
            depth++;
            const char *n = (const char *)p;
            p = (const uint32_t *)FDT_ALIGN((uintptr_t)(n + strlen(n) + 1), 4);
        } else if (tag == FDT_END_NODE) {
            if (depth == 0) {
                break;
            }
            depth--;
        } else if (tag == FDT_PROP) {
            const struct fdt_prop_header *hdr = (const struct fdt_prop_header *)p;
            uint32_t len = fdt32_to_cpu(hdr->len);
            const char *cur_name = fdt_strings_block + fdt32_to_cpu(hdr->nameoff);
            const void *value = (const void *)(hdr + 1);
            p = (const uint32_t *)FDT_ALIGN((uintptr_t)value + len, 4);

            if (depth == 0 && strcmp(cur_name, prop_name) == 0) {
                if (out_prop) {
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

/*
 * fdt_find_node_by_compatible - Locates the first node matching a compatible string.
 */
const uint32_t *fdt_find_node_by_compatible(const char *compatible)
{
    const uint32_t *p = fdt_struct_block;
    uint32_t tag;
    const uint32_t *current_node = NULL;

    while ((tag = fdt_next_tag(&p)) != FDT_END) {
        if (tag == FDT_BEGIN_NODE) {
            current_node = p - 1;
            const char *name = (const char *)p;
            p = (const uint32_t *)FDT_ALIGN((uintptr_t)(name + strlen(name) + 1), 4);
        } else if (tag == FDT_PROP) {
            const struct fdt_prop_header *hdr = (const struct fdt_prop_header *)p;
            uint32_t len = fdt32_to_cpu(hdr->len);
            const char *cur_name = fdt_strings_block + fdt32_to_cpu(hdr->nameoff);
            const char *value = (const char *)(hdr + 1);
            p = (const uint32_t *)FDT_ALIGN((uintptr_t)value + len, 4);

            if (strcmp(cur_name, "compatible") == 0) {
                const char *s = value;
                /* Match any string in the compatibility list */
                for (size_t read = 0; read < len; read += strlen(s) + 1, s += strlen(s) + 1) {
                    if (strcmp(s, compatible) == 0) {
                        return current_node;
                    }
                }
            }
        }
    }
    return NULL;
}

/*
 * fdt_find_node_by_path - Resolves a node pointer from an absolute path.
 */
const uint32_t *fdt_find_node_by_path(const char *path)
{
    if (!path || path[0] != '/') {
        return NULL;
    }

    const uint32_t *p = fdt_struct_block;
    uint32_t tag;

    if (path[1] == '\0') {
        return p;
    }

    const char *current_comp = path + 1;
    int current_depth = 0;
    int search_depth = 1;

    while ((tag = fdt_next_tag(&p)) != FDT_END) {
        if (tag == FDT_BEGIN_NODE) {
            const uint32_t *node_start = p - 1;
            const char *node_name = (const char *)p;
            p = (const uint32_t *)FDT_ALIGN((uintptr_t)(node_name + strlen(node_name) + 1), 4);

            current_depth++;

            if (current_depth == 1) {
                if (node_name[0] == '\0') {
                    search_depth = 2;
                    continue;
                } else {
                    return NULL;
                }
            }

            if (current_depth == search_depth) {
                const char *next_slash = strchr(current_comp, '/');
                size_t comp_len =
                    next_slash ? (size_t)(next_slash - current_comp) : strlen(current_comp);

                size_t node_name_len = 0;
                while (node_name[node_name_len] != '\0' && node_name[node_name_len] != '@') {
                    node_name_len++;
                }

                size_t comp_name_len = 0;
                while (current_comp[comp_name_len] != '\0' && current_comp[comp_name_len] != '@'
                       && current_comp[comp_name_len] != '/') {
                    comp_name_len++;
                }

                int match = 0;
                if (comp_name_len == node_name_len
                    && strncmp(current_comp, node_name, comp_name_len) == 0) {
                    const char *comp_unit = strchr(current_comp, '@');
                    if (comp_unit && comp_unit < current_comp + comp_len) {
                        if (strncmp(current_comp, node_name, comp_len) == 0
                            && node_name[comp_len] == '\0') {
                            match = 1;
                        }
                    } else {
                        match = 1;
                    }
                }

                if (match) {
                    if (!next_slash || next_slash[1] == '\0') {
                        return node_start;
                    }
                    current_comp = next_slash + 1;
                    search_depth++;
                    continue;
                }
            }

            /* Skip subtree if component didn't match */
            int skip_depth = 1;
            while (skip_depth > 0 && (tag = fdt_next_tag(&p)) != FDT_END) {
                if (tag == FDT_BEGIN_NODE) {
                    skip_depth++;
                    const char *n = (const char *)p;
                    p = (const uint32_t *)FDT_ALIGN((uintptr_t)(n + strlen(n) + 1), 4);
                } else if (tag == FDT_END_NODE) {
                    skip_depth--;
                } else if (tag == FDT_PROP) {
                    const struct fdt_prop_header *hdr = (const struct fdt_prop_header *)p;
                    p = (const uint32_t *)FDT_ALIGN((uintptr_t)(hdr + 1) + fdt32_to_cpu(hdr->len),
                                                    4);
                }
            }
            current_depth--;
        } else if (tag == FDT_END_NODE) {
            current_depth--;
        } else if (tag == FDT_PROP) {
            const struct fdt_prop_header *hdr = (const struct fdt_prop_header *)p;
            p = (const uint32_t *)FDT_ALIGN((uintptr_t)(hdr + 1) + fdt32_to_cpu(hdr->len), 4);
        }
    }

    return NULL;
}

/*
 * fdt_get_parent_node - Locates the parent node of a given node.
 */
const uint32_t *fdt_get_parent_node(const uint32_t *target_node)
{
    if (!target_node || target_node == fdt_struct_block) {
        return NULL;
    }

    const uint32_t *p = fdt_struct_block;
    uint32_t tag;

    const uint32_t *parent_stack[32];
    int depth = 0;

    while ((tag = fdt_next_tag(&p)) != FDT_END) {
        if (tag == FDT_BEGIN_NODE) {
            const uint32_t *current_node = p - 1;

            if (current_node == target_node) {
                if (depth > 0) {
                    return parent_stack[depth - 1];
                }
                return NULL;
            }

            if (depth < 32) {
                parent_stack[depth] = current_node;
            }
            depth++;

            const char *name = (const char *)p;
            p = (const uint32_t *)FDT_ALIGN((uintptr_t)(name + strlen(name) + 1), 4);
        } else if (tag == FDT_END_NODE) {
            if (depth > 0) {
                depth--;
            }
        } else if (tag == FDT_PROP) {
            const struct fdt_prop_header *hdr = (const struct fdt_prop_header *)p;
            p = (const uint32_t *)FDT_ALIGN((uintptr_t)(hdr + 1) + fdt32_to_cpu(hdr->len), 4);
        }
    }

    return NULL;
}
