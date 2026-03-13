#include "elf.h"
#include "uapi/errors.h"
#include "vfs.h"
#include "pmm.h"
#include "mmu.h"
#include "addr.h"
#include "heap.h"
#include "process.h"
#include "string.h"
#include "stdio.h"
#include "panic.h"

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

static int elf_check_header(Elf64_Ehdr* hdr)
{
    if (hdr->e_ident[EI_MAG0] != ELFMAG0 || hdr->e_ident[EI_MAG1] != ELFMAG1 || hdr->e_ident[EI_MAG2] != ELFMAG2 ||
        hdr->e_ident[EI_MAG3] != ELFMAG3)
    {
        return -PERS_ERR_EXECUTABLE_FORMAT_ERROR;
    }
    if (hdr->e_ident[EI_CLASS] != ELFCLASS64)
        return -PERS_ERR_EXECUTABLE_FORMAT_ERROR;
    if (hdr->e_ident[EI_DATA] != ELFDATA2LSB)
        return -PERS_ERR_EXECUTABLE_FORMAT_ERROR;
    if (hdr->e_type != ET_EXEC)
        return -PERS_ERR_EXECUTABLE_FORMAT_ERROR;
    if (hdr->e_machine != EM_AARCH64)
        return -PERS_ERR_EXECUTABLE_FORMAT_ERROR;
    return PERS_SUCCESS;
}

int elf_load(const char* path, unsigned long* pgd, uint64_t* entry_point)
{
    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0)
    {
        printf("[  ELF ] Error: could not open %s\n", path);
        return fd;
    }

    Elf64_Ehdr ehdr;
    int rc = vfs_read(fd, &ehdr, sizeof(Elf64_Ehdr));
    if (rc != sizeof(Elf64_Ehdr))
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

    *entry_point = ehdr.e_entry;

    size_t phdr_table_size = ehdr.e_phnum * sizeof(Elf64_Phdr);
    Elf64_Phdr* phdrs = kmalloc(phdr_table_size);
    if (!phdrs)
    {
        printf("[  ELF ] Error: could not allocate memory for program headers\n");
        vfs_close(fd);
        return -PERS_ERR_OUT_OF_MEMORY;
        ;
    }

    vfs_lseek(fd, ehdr.e_phoff, SEEK_SET);
    if (vfs_read(fd, phdrs, phdr_table_size) != (int)phdr_table_size)
    {
        printf("[  ELF ] Error: could not read program headers\n");
        kfree(phdrs);
        vfs_close(fd);
        return -PERS_ERR_EXECUTABLE_FORMAT_ERROR;
    }

    for (int i = 0; i < ehdr.e_phnum; i++)
    {
        if (phdrs[i].p_type != PT_LOAD)
            continue;

        uint64_t vaddr = phdrs[i].p_vaddr;
        uint64_t memsz = phdrs[i].p_memsz;
        uint64_t filesz = phdrs[i].p_filesz;
        uint64_t offset = phdrs[i].p_offset;
        uint32_t flags = phdrs[i].p_flags;

        uint64_t start_vpage = vaddr & ~0xFFFULL;
        uint64_t end_vpage = (vaddr + memsz + 0xFFFULL) & ~0xFFFULL;

        unsigned long mmu_flags = (flags & PF_X) ? PAGE_USER_CODE : PAGE_USER_DATA;

        for (uint64_t page = start_vpage; page < end_vpage; page += PAGE_SIZE)
        {
            void* kernel_vaddr;
            unsigned long current_paddr;
            unsigned long current_flags;
            int is_mapped = mmu_user_query(pgd, page, &current_paddr, &current_flags);

            if (is_mapped)
            {
                kernel_vaddr = (void*)P2V(current_paddr);
                // upgrade permissions if needed (e.g. add write bit)
                if ((mmu_flags & PAGE_USER_DATA) && !(current_flags & MMU_UXN))
                {
                    // this is a simplified permission merge
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
                    kfree(phdrs);
                    vfs_close(fd);
                    return -PERS_ERR_IO_ERROR;
                }
            }

            if (flags & PF_X)
            {
                flush_icache_range(kernel_vaddr, PAGE_SIZE);
            }
        }
    }

    kfree(phdrs);
    vfs_close(fd);
    return PERS_SUCCESS;
}
