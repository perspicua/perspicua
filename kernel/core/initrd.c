/*
 * initrd.c - Implementation of the Initial RAM Disk (InitRD) parser.
 *
 * This module parses the boot-time CPIO archive and populates the root
 * filesystem (RAMFS) with essential system files.
 */

#include "core/initrd.h"

#include "fs/ramfs.h"
#include "string.h"
#include "stdio.h"

/* --- Private Helper Functions --- */

/*
 * hex8_to_u32 - Converts an 8-char hex string to a 32-bit integer.
 */
static uint32_t hex8_to_u32(const char *s)
{
    uint32_t res = 0;
    for (int i = 0; i < 8; i++) {
        res <<= 4;
        char c = s[i];
        if (c >= '0' && c <= '9') {
            res += (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            res += (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            res += (uint32_t)(c - 'A' + 10);
        }
    }
    return res;
}

/* --- Public API Implementations --- */

/*
 * initrd_init - Iterates through the CPIO archive and registers files.
 */
void initrd_init(void *initrd_start)
{
    char *ptr = (char *)initrd_start;
    int count = 0;

    while (1) {
        struct cpio_newc_header *hdr = (struct cpio_newc_header *)ptr;

        if (memcmp(hdr->magic, "070701", 6) != 0) {
            pr_err("boot: invalid CPIO magic at %p\n", ptr);
            break;
        }

        uint32_t name_size = hex8_to_u32(hdr->name_size);
        uint32_t file_size = hex8_to_u32(hdr->file_size);
        uint32_t mode = hex8_to_u32(hdr->mode);
        char *filename = ptr + sizeof(struct cpio_newc_header);

        if (strcmp(filename, "TRAILER!!!") == 0) {
            break;
        }

        /* Skip header and filename, account for 4-byte padding */
        char *data = ptr + sizeof(struct cpio_newc_header) + name_size;
        data = (char *)(((uintptr_t)data + 3) & ~3UL);

        /* Only S_IFREG (0x8000) files are currently registered */
        if ((mode & 0xF000) == 0x8000) {
            ramfs_register_file(filename, data, file_size);
            count++;
        }

        /* Account for 4-byte padding after file data */
        ptr = data + file_size;
        ptr = (char *)(((uintptr_t)ptr + 3) & ~3UL);
    }

    pr_info("boot: initrd parsed: %d files registered\n", count);
}
