/*
 * elf.c - Implementation of the ELF executable loader.
 *
 * This file handles parsing of the ELF header, program headers, and
 * loading segments into the user's virtual address space.
 */

#include "elf.h"

#include "uapi/errors.h"

#include "pmm.h"
#include "mmu.h"
#include "vfs.h"
#include "addr.h"
#include "heap.h"
#include "process.h"
#include "string.h"
#include "stdio.h"
#include "panic.h"

/* Program header flag constants */
#define ELF_PROG_FLAG_X 0x1
#define ELF_PROG_FLAG_W 0x2
#define ELF_PROG_FLAG_R 0x4

/*
 * elf_check_header - Validates the ELF file identity and machine type.
 */
static int elf_check_header(struct elf64_header* hdr)
{
    if (hdr->identity[ELF_IDENT_MAG0] != ELF_MAG0 || hdr->identity[ELF_IDENT_MAG1] != ELF_MAG1 ||
        hdr->identity[ELF_IDENT_MAG2] != ELF_MAG2 || hdr->identity[ELF_IDENT_MAG3] != ELF_MAG3)
    {
        return -PERS_ERR_EXECUTABLE_FORMAT_ERROR;
    }

    if (hdr->identity[ELF_IDENT_CLASS] != ELF_CLASS_64)
    {
        return -PERS_ERR_EXECUTABLE_FORMAT_ERROR;
    }

    if (hdr->identity[ELF_IDENT_DATA] != ELF_DATA_2LSB)
    {
        return -PERS_ERR_EXECUTABLE_FORMAT_ERROR;
    }

    if (hdr->type != ELF_TYPE_EXEC)
    {
        return -PERS_ERR_EXECUTABLE_FORMAT_ERROR;
    }

    if (hdr->machine != ELF_MACHINE_AARCH64)
    {
        return -PERS_ERR_EXECUTABLE_FORMAT_ERROR;
    }

    return PERS_SUCCESS;
}

/*
 * elf_load - Implementation of the ELF loading process.
 */
int elf_load(const char* path, unsigned long* pgd, uint64_t* entry_point)
{
    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0)
    {
        printf("[  ELF ] Error: could not open %s\n", path);
        return fd;
    }

    struct elf64_header ehdr;
    int rc = vfs_read(fd, &ehdr, sizeof(struct elf64_header));
    if (rc != sizeof(struct elf64_header))
    {
        printf("[  ELF ] Error: could not read ELF header\n");
        vfs_close(fd);
        return -PERS_ERR_EXECUTABLE_FORMAT_ERROR;
    }

    rc = elf_check_header(&ehdr);
    if (rc != 0)
    {
        printf("[  ELF ] Error: invalid ELF header\n");
        vfs_close(fd);
        return rc;
    }

    *entry_point = ehdr.entry;

    size_t phdr_table_size = ehdr.ph_num * sizeof(struct elf64_program_header);
    struct elf64_program_header* phdrs = heap_malloc(phdr_table_size);
    if (!phdrs)
    {
        printf("[  ELF ] Error: could not allocate memory for program headers\n");
        vfs_close(fd);
        return -PERS_ERR_OUT_OF_MEMORY;
    }

    vfs_lseek(fd, ehdr.ph_offset, SEEK_SET);
    if (vfs_read(fd, phdrs, phdr_table_size) != (int)phdr_table_size)
    {
        printf("[  ELF ] Error: could not read program headers\n");
        heap_free(phdrs);
        vfs_close(fd);
        return -PERS_ERR_EXECUTABLE_FORMAT_ERROR;
    }

    for (int i = 0; i < ehdr.ph_num; i++)
    {
        if (phdrs[i].type != ELF_PROG_LOAD)
        {
            continue;
        }

        uint64_t vaddr = phdrs[i].vaddr;
        uint64_t memsz = phdrs[i].mem_size;
        uint64_t filesz = phdrs[i].file_size;
        uint64_t offset = phdrs[i].offset;
        uint32_t flags = phdrs[i].flags;

        uint64_t start_vpage = vaddr & ~0xFFFULL;
        uint64_t end_vpage = (vaddr + memsz + 0xFFFULL) & ~0xFFFULL;

        unsigned long mmu_flags = (flags & ELF_PROG_FLAG_X) ? MMU_PAGE_USER_CODE : MMU_PAGE_USER_DATA;

        for (uint64_t page = start_vpage; page < end_vpage; page += PAGE_SIZE)
        {
            void* kernel_vaddr;
            unsigned long current_paddr;
            unsigned long current_flags;
            int is_mapped = mmu_user_query(pgd, page, &current_paddr, &current_flags);

            if (is_mapped)
            {
                kernel_vaddr = (void*)P2V(current_paddr);
                // Adjust permissions if needed when mapping memory for data
                if ((mmu_flags & MMU_PAGE_USER_DATA) && !(current_flags & MMU_UXN))
                {
                    mmu_user_unmap_page(pgd, page);
                    mmu_user_map_page(pgd, page, current_paddr, current_flags | mmu_flags);
                }
            }
            else
            {
                kernel_vaddr = pmm_alloc_page();
                if (!kernel_vaddr)
                {
                    PANIC("ELF: Out of memory during loading");
                }
                memset(kernel_vaddr, 0, PAGE_SIZE);
                mmu_user_map_page(pgd, page, V2P(kernel_vaddr), mmu_flags);
            }

            uint64_t page_end = page + PAGE_SIZE;
            uint64_t copy_start_in_page = (vaddr > page) ? (vaddr - page) : 0;
            uint64_t segment_end_vaddr = vaddr + filesz;
            uint64_t copy_end_in_page;

            if (segment_end_vaddr <= page)
            {
                copy_end_in_page = 0;
            }
            else if (segment_end_vaddr >= page_end)
            {
                copy_end_in_page = PAGE_SIZE;
            }
            else
            {
                copy_end_in_page = segment_end_vaddr - page;
            }

            if (copy_start_in_page < copy_end_in_page)
            {
                uint64_t bytes_to_read = copy_end_in_page - copy_start_in_page;
                uint64_t file_offset = offset + (page + copy_start_in_page - vaddr);

                vfs_lseek(fd, file_offset, SEEK_SET);
                if (vfs_read(fd, (void*)((uintptr_t)kernel_vaddr + copy_start_in_page), bytes_to_read) !=
                    (int)bytes_to_read)
                {
                    printf("[  ELF ] Error: failed to read segment data\n");
                    heap_free(phdrs);
                    vfs_close(fd);
                    return -PERS_ERR_IO_ERROR;
                }
            }

            if (flags & ELF_PROG_FLAG_X)
            {
                process_flush_icache_range(kernel_vaddr, PAGE_SIZE);
            }
        }
    }

    heap_free(phdrs);
    vfs_close(fd);
    return PERS_SUCCESS;
}
