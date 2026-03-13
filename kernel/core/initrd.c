#include "initrd.h"
#include "ramfs.h"
#include "string.h"
#include "stdio.h"

static uint32_t hex8_to_u32(const char* s)
{
    uint32_t res = 0;
    for (int i = 0; i < 8; i++)
    {
        res <<= 4;
        char c = s[i];
        if (c >= '0' && c <= '9')
            res += (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            res += (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            res += (uint32_t)(c - 'A' + 10);
    }
    return res;
}

void initrd_init(void* initrd_start)
{
    char* ptr = (char*)initrd_start;

    while (1)
    {
        struct cpio_newc_header* hdr = (struct cpio_newc_header*)ptr;

        if (memcmp(hdr->c_magic, "070701", 6) != 0)
        {
            printf("[  BOOT ] Error: Invalid CPIO magic\n");
            break;
        }

        uint32_t namesize = hex8_to_u32(hdr->c_namesize);
        uint32_t filesize = hex8_to_u32(hdr->c_filesize);
        uint32_t mode = hex8_to_u32(hdr->c_mode);

        char* filename = ptr + sizeof(struct cpio_newc_header);

        if (strcmp(filename, "TRAILER!!!") == 0)
            break;

        char* data = ptr + sizeof(struct cpio_newc_header) + namesize;
        data = (char*)(((uintptr_t)data + 3) & ~3UL);

        if ((mode & 0xF000) == 0x8000)
            ramfs_register_file(filename, data, filesize);

        ptr = data + filesize;
        ptr = (char*)(((uintptr_t)ptr + 3) & ~3UL);
    }
    printf("[  BOOT ] InitRD parsed and mounted to RAMFS\n");
}
