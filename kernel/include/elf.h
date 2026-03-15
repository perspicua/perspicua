/*
 * elf.h - Public API for the Executable and Linkable Format (ELF) parser.
 *
 * This file defines the structures and constants for the 64-bit ELF format
 * as used by the kernel to load and execute user applications.
 */

#ifndef PERSPICUA_KERNEL_ELF_H
#define PERSPICUA_KERNEL_ELF_H

#include "types.h"

/* ELF Identification indexes */
#define ELF_IDENT_MAG0       0
#define ELF_IDENT_MAG1       1
#define ELF_IDENT_MAG2       2
#define ELF_IDENT_MAG3       3
#define ELF_IDENT_CLASS      4
#define ELF_IDENT_DATA       5
#define ELF_IDENT_VERSION    6
#define ELF_IDENT_OSABI      7
#define ELF_IDENT_ABIVERSION 8
#define ELF_IDENT_PAD        9
#define ELF_IDENT_NIDENT     16

/* ELF Magic numbers */
#define ELF_MAG0 0x7f
#define ELF_MAG1 'E'
#define ELF_MAG2 'L'
#define ELF_MAG3 'F'

/* ELF Class and Data constants */
#define ELF_CLASS_64  2
#define ELF_DATA_2LSB 1

/* ELF Type and Machine constants */
#define ELF_TYPE_EXEC       2
#define ELF_MACHINE_AARCH64 183

/* Program Header Type constants */
#define ELF_PROG_LOAD 1

/* Standard ELF 64-bit types */
typedef uint64_t elf64_addr;
typedef uint64_t elf64_off;
typedef uint16_t elf64_half;
typedef uint32_t elf64_word;
typedef int32_t elf64_sword;
typedef uint64_t elf64_xword;
typedef int64_t elf64_sxword;

/*
 * elf64_header - The main ELF file header structure.
 */
struct elf64_header
{
    unsigned char identity[ELF_IDENT_NIDENT];
    elf64_half type;
    elf64_half machine;
    elf64_word version;
    elf64_addr entry;
    elf64_off ph_offset;
    elf64_off sh_offset;
    elf64_word flags;
    elf64_half eh_size;
    elf64_half ph_entry_size;
    elf64_half ph_num;
    elf64_half sh_entry_size;
    elf64_half sh_num;
    elf64_half sh_str_ndx;
};

/*
 * elf64_program_header - Represents a segment in the ELF file.
 */
struct elf64_program_header
{
    elf64_word type;
    elf64_word flags;
    elf64_off offset;
    elf64_addr vaddr;
    elf64_addr paddr;
    elf64_xword file_size;
    elf64_xword mem_size;
    elf64_xword align;
};

/*
 * elf_load - Parses and loads an ELF executable from the filesystem into
 * the provided page directory. It returns PERS_SUCCESS on success or an
 * error code on failure, and populates the entry_point with the start address.
 */
int elf_load(const char* path, unsigned long* pgd, uint64_t* entry_point);

#endif /* PERSPICUA_KERNEL_ELF_H */
