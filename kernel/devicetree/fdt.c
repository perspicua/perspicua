#include "../include/devicetree/fdt.h"
#include "panic.h"
#include "addr.h"
#include "../../libc/include/string.h" // For strlen (assuming libc is available)

// Global pointers for the structure and strings blocks.
// They are marked static because they only need to be accessed within this file.
static const uint32_t *fdt_struct_block;
static const char *fdt_strings_block;
static uintptr_t fdt_base_address;

/* 
 * Helper to get the next 32-bit token and advance the pointer.
 * The pointer p must be aligned to 4 bytes.
 */
static inline uint32_t fdt_next_tag(const uint32_t **p)
{
    uint32_t tag = fdt32_to_cpu(**p);
    (*p)++;
    return tag;
}

/*
 * Step 4 Primitive: A generic parser function that iterates through a single node's
 * properties and child nodes, given a starting pointer.
 * This function handles exactly one node (from BEGIN_NODE to its matching END_NODE).
 * For now, this just traverses it, but later it can execute search callbacks.
 */
static void fdt_parse_node(const uint32_t **p)
{
    uint32_t tag;
    
    // The current pointer must be pointing right after a BEGIN_NODE token.
    // The node name immediately follows the BEGIN_NODE token.
    const char *node_name = (const char *)*p;
    
    // Advance pointer past the name string. All strings in FDT are null-terminated 
    // and padded to a 4-byte boundary.
    size_t name_len = 0;
    while (node_name[name_len] != '\0') {
        name_len++;
    }
    
    // Align address to next 4-byte boundary correctly
    uintptr_t align_addr = FDT_ALIGN((uintptr_t)(node_name + name_len + 1), 4);
    *p = (const uint32_t *)align_addr;

    // Loop through the body of the node
    while ((tag = fdt_next_tag(p)) != FDT_END) {
        if (tag == FDT_BEGIN_NODE) {
            // Found a child node, recursively parse it
            fdt_parse_node(p);
        }
        else if (tag == FDT_END_NODE) {
            // End of the current node, resume checking at the parent level
            break;
        }
        else if (tag == FDT_PROP) {
            // Found a property string. It starts with the prop header.
            const struct fdt_prop_header *prop_hdr = (const struct fdt_prop_header *)*p;
            uint32_t len = fdt32_to_cpu(prop_hdr->len);
            uint32_t nameoff = fdt32_to_cpu(prop_hdr->nameoff);
            
            // Reconstruct the actual property wrapper
            struct fdt_property prop;
            prop.name = fdt_strings_block + nameoff;
            prop.size = len;
            prop.value = (const void *)(prop_hdr + 1); // Value starts right after the header
            
            // Advance the pointer past the header and the property value, aligned to 4 bytes.
            uintptr_t next_addr = FDT_ALIGN((uintptr_t)prop.value + prop.size, 4);
            *p = (const uint32_t *)next_addr;
            
            // (In future steps, we can check prop.name here and extract values like "reg")
        }
        else if (tag == FDT_NOP) {
            // Ignore NOPs and continue
            continue;
        }
    }
}

void fdt_init(uintptr_t global_dtb_ptr)
{
    // For this simple implementation, we just store the pointer to the DTB
    // in a global variable for later use by other subsystems.
    // fdt_global_dtb_ptr = global_dtb_ptr;
    struct fdt_header *fdt = (struct fdt_header *)global_dtb_ptr;
    if(fdt32_to_cpu(fdt->magic) != FDT_MAGIC)
    {
        // Invalid DTB, handle error (e.g., panic or fallback)
        PANIC("Invalid FDT: magic number mismatch");
    }
    uint32_t structure_offset = fdt32_to_cpu(fdt->off_dt_struct);
    uint32_t strings_offset = fdt32_to_cpu(fdt->off_dt_strings);
    // Store these offsets in global variables if needed for later parsing
    fdt_base_address = global_dtb_ptr;
    fdt_struct_block = (const uint32_t *)(fdt_base_address + structure_offset);
    fdt_strings_block = (const char *)(fdt_base_address + strings_offset);
}

void fdt_rebase(uintptr_t new_base)
{
    struct fdt_header *fdt = (struct fdt_header *)new_base;
    uint32_t structure_offset = fdt32_to_cpu(fdt->off_dt_struct);
    uint32_t strings_offset   = fdt32_to_cpu(fdt->off_dt_strings);

    fdt_base_address  = new_base;
    fdt_struct_block  = (const uint32_t *)(new_base + structure_offset);
    fdt_strings_block = (const char *)(new_base + strings_offset);
}

/*
 * Reads the memory reservation block from the FDT header and
 * informs the PMM to avoid allocating these physical regions.
 * Also reserves the physical memory occupied by the DTB itself.
 */
void fdt_parse_memory_reservations(void)
{
    struct fdt_header *fdt = (struct fdt_header *)fdt_base_address;
    
    // 1. Reserve the DTB itself so PMM doesn't overwrite our tree
    uint32_t totalsize = fdt32_to_cpu(fdt->totalsize);
    extern void pmm_reserve_range(unsigned long phys_start, unsigned long size, const char* tag);
    pmm_reserve_range((unsigned long)fdt_base_address, totalsize, "dtb");

    // 2. Parse the official memory reservation map
    uint32_t rsv_offset = fdt32_to_cpu(fdt->off_mem_rsvmap);
    extern void printf(const char* fmt, ...);
    
    // Bounds check: rsvmap must be within the DTB
    if (rsv_offset >= totalsize) {
        printf("[  DTB ] WARNING: rsvmap offset out of bounds!\n");
        return;
    }

    const struct fdt_reserve_entry *rsvmap = (const struct fdt_reserve_entry *)(fdt_base_address + rsv_offset);
    
    int count = 0;
    while (count++ < 64) {
        uint64_t addr = fdt64_to_cpu(rsvmap->address);
        uint64_t size = fdt64_to_cpu(rsvmap->size);
        
        printf("[  DTB ] rsvmap[%d]: base=0x%lx size=0x%lx\n", count-1, addr, size);
        
        if (size == 0 && addr == 0) {
            break;
        }
        
        if (addr < 0x40000000UL) {
            pmm_reserve_range((unsigned long)addr, (unsigned long)size, "dtb-reserved");
        } else {
            printf("[  DTB ] WARNING: skipping out-of-range reservation!\n");
        }
        
        rsvmap++;
    }
}

/*
 * Scans the properties of a given node token to find a property by name.
 * `node` must point to the FDT_BEGIN_NODE token of the target node.
 */
int fdt_get_property(const uint32_t *node, const char *prop_name, struct fdt_property *out_prop)
{
    if (fdt32_to_cpu(*node) != FDT_BEGIN_NODE) {
        return -1;
    }
    
    // Skip the BEGIN_NODE token
    const uint32_t *p = node + 1;
    
    // Skip the node name
    const char *node_name = (const char *)p;
    size_t name_len = strlen(node_name);
    p = (const uint32_t *)FDT_ALIGN((uintptr_t)(node_name + name_len + 1), 4);

    uint32_t tag;
    int depth = 0; // Depth relative to the target node

    while ((tag = fdt_next_tag(&p)) != FDT_END) {
        if (tag == FDT_BEGIN_NODE) {
            depth++;
            // Skip inner node name
            const char *inner_name = (const char *)p;
            p = (const uint32_t *)FDT_ALIGN((uintptr_t)(inner_name + strlen(inner_name) + 1), 4);
        } 
        else if (tag == FDT_END_NODE) {
            if (depth == 0) {
                break; // End of our target node
            }
            depth--;
        } 
        else if (tag == FDT_PROP) {
            const struct fdt_prop_header *prop_hdr = (const struct fdt_prop_header *)p;
            uint32_t len = fdt32_to_cpu(prop_hdr->len);
            uint32_t nameoff = fdt32_to_cpu(prop_hdr->nameoff);
            const char *current_prop_name = fdt_strings_block + nameoff;
            const void *prop_value = (const void *)(prop_hdr + 1);
            
            // Advance pointer past property data
            p = (const uint32_t *)FDT_ALIGN((uintptr_t)prop_value + len, 4);
            
            // We only care about properties that belong directly to our node (depth == 0)
            if (depth == 0 && strcmp(current_prop_name, prop_name) == 0) {
                if (out_prop) {
                    out_prop->name = current_prop_name;
                    out_prop->size = len;
                    out_prop->value = prop_value;
                }
                return 0; // Found
            }
        }
    }
    return -1; // Property not found
}

/*
 * Linearly iterates through all nodes in the DTB to find one with a matching compatible string.
 * Returns a pointer to the FDT_BEGIN_NODE token.
 */
const uint32_t *fdt_find_node_by_compatible(const char *compatible)
{
    const uint32_t *p = fdt_struct_block;
    uint32_t tag;
    const uint32_t *current_node_token = NULL;

    while ((tag = fdt_next_tag(&p)) != FDT_END) {
        if (tag == FDT_BEGIN_NODE) {
            current_node_token = p - 1; // Save pointer to the token
            
            const char *node_name = (const char *)p;
            p = (const uint32_t *)FDT_ALIGN((uintptr_t)(node_name + strlen(node_name) + 1), 4);
        } 
        else if (tag == FDT_PROP) {
            const struct fdt_prop_header *prop_hdr = (const struct fdt_prop_header *)p;
            uint32_t len = fdt32_to_cpu(prop_hdr->len);
            uint32_t nameoff = fdt32_to_cpu(prop_hdr->nameoff);
            const char *prop_name = fdt_strings_block + nameoff;
            const char *prop_value = (const char *)(prop_hdr + 1);
            
            p = (const uint32_t *)FDT_ALIGN((uintptr_t)prop_value + len, 4);
            
            if (strcmp(prop_name, "compatible") == 0) {
                // compatible is a list of null-terminated strings
                const char *list_ptr = prop_value;
                size_t chars_read = 0;
                while (chars_read < len) {
                    if (strcmp(list_ptr, compatible) == 0) {
                        return current_node_token; // Match found
                    }
                    size_t str_len = strlen(list_ptr) + 1;
                    list_ptr += str_len;
                    chars_read += str_len;
                }
            }
        }
    }
    return NULL;
}

/*
 * A simplified path matching utility.
 * Iterates through the tree linearly, tracking the names at each depth.
 */
const uint32_t *fdt_find_node_by_path(const char *path)
{
    const uint32_t *p = fdt_struct_block;
    uint32_t tag;
    
    // We maintain a stack of node names to track our current absolute path.
    const char *path_stack[16]; 
    int depth = -1; // -1 means before the root node

    while ((tag = fdt_next_tag(&p)) != FDT_END) {
        if (tag == FDT_BEGIN_NODE) {
            const uint32_t *current_token = p - 1;
            const char *node_name = (const char *)p;
            p = (const uint32_t *)FDT_ALIGN((uintptr_t)(node_name + strlen(node_name) + 1), 4);
            
            depth++;
            if (depth < 16) {
                // Root node has an empty name, but its path prefix is implicitly /
                path_stack[depth] = (depth == 0) ? "" : node_name;
                
                // Reconstruct current absolute path
                char current_path[256];
                current_path[0] = '\0';
                for (int i = 0; i <= depth; i++) {
                    if (i == 0) {
                        strcat(current_path, "/");
                    } else {
                        strcat(current_path, path_stack[i]);
                        // Only add trailing slash if it is not the last component
                        if (i != depth) {
                            strcat(current_path, "/");
                        }
                    }
                }
                
                // Deal with double slashes if root node name was empty
                if (current_path[1] == '\0' && strcmp(path, "/") == 0) {
                    return current_token;
                }
                
                if (strcmp(current_path, path) == 0) {
                    return current_token;
                }
            }
        } 
        else if (tag == FDT_END_NODE) {
            depth--;
        } 
        else if (tag == FDT_PROP) {
            const struct fdt_prop_header *prop_hdr = (const struct fdt_prop_header *)p;
            uint32_t len = fdt32_to_cpu(prop_hdr->len);
            p = (const uint32_t *)FDT_ALIGN((uintptr_t)(prop_hdr + 1) + len, 4);
        }
    }
    return NULL;
}
