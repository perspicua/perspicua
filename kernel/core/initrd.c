/*
 * initrd.c - Implementation of the Initial RAM Disk (InitRD) parser.
 *
 * This file handles parsing the CPIO 'newc' format, converting metadata
 * from hexadecimal ASCII strings to integers, and registering the
 * discovered files into the RAM filesystem.
 */

#ifndef PERSPICUA_KERNEL_INITRD_H
    #include "initrd.h"
#endif

#include "ramfs.h"
#include "string.h"
#include "stdio.h"

/*
 * hex8_to_u32 - Converts an 8-character hexadecimal ASCII string to a 32-bit
 * unsigned integer.
 */
static uint32_t hex8_to_u32(const char* s)
{
    uint32_t res = 0;
    for (int i = 0; i < 8; i++)
    {
        res <<= 4;
        char c = s[i];
        if (c >= '0' && c <= '9')
        {
            res += (uint32_t)(c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            res += (uint32_t)(c - 'a' + 10);
        }
        else if (c >= 'A' && c <= 'F')
        {
            res += (uint32_t)(c - 'A' + 10);
        }
    }
    return res;
}

/*
 * initrd_init - Iterates through the CPIO archive in memory.
 */
void initrd_init(void* initrd_start)
{
    char* ptr = (char*)initrd_start;

    while (1)
    {
        struct cpio_newc_header* hdr = (struct cpio_newc_header*)ptr;

        /* Validate the CPIO 'newc' magic number */
        if (memcmp(hdr->magic, "070701", 6) != 0)
        {
            printf("[  BOOT ] Error: Invalid CPIO magic\n");
            break;
        }

        uint32_t name_size = hex8_to_u32(hdr->name_size);
        uint32_t file_size = hex8_to_u32(hdr->file_size);
        uint32_t mode      = hex8_to_u32(hdr->mode);

        char* filename = ptr + sizeof(struct cpio_newc_header);

        /* The TRAILER!!! record marks the end of the CPIO archive */
        if (strcmp(filename, "TRAILER!!!") == 0)
        {
            break;
        }

        /* Calculate data offset, accounting for 4-byte padding after the filename */
        char* data = ptr + sizeof(struct cpio_newc_header) + name_size;
        data       = (char*)(((uintptr_t)data + 3) & ~3UL);

        /* Only register regular files into the RAMFS */
        if ((mode & 0xF000) == 0x8000)
        {
            ramfs_register_file(filename, data, file_size);
        }

        /* Move to the next header, accounting for 4-byte padding after the data */
        ptr = data + file_size;
        ptr = (char*)(((uintptr_t)ptr + 3) & ~3UL);
    }

    printf("[  BOOT ] InitRD parsed and mounted to RAMFS\n");
}
