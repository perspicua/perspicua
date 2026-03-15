/*
 * initrd.h - Public API for the Initial RAM Disk (InitRD) parser.
 *
 * This file defines the structures and functions used to parse the CPIO
 * archive containing the initial filesystem and system programs.
 */

#ifndef PERSPICUA_KERNEL_INITRD_H
#define PERSPICUA_KERNEL_INITRD_H

#include "types.h"

/*
 * cpio_newc_header - Represents a header in the CPIO 'newc' format.
 * All fields are 8-byte hexadecimal ASCII strings, except magic which is 6-byte.
 */
struct cpio_newc_header
{
    char magic[6];
    char inode[8];
    char mode[8];
    char uid[8];
    char gid[8];
    char nlink[8];
    char mtime[8];
    char file_size[8];
    char dev_major[8];
    char dev_minor[8];
    char rdev_major[8];
    char rdev_minor[8];
    char name_size[8];
    char checksum[8];
} __attribute__((packed));

/*
 * initrd_init - Parses the CPIO archive at the given memory address and
 * registers all files found within it into the RAMFS.
 */
void initrd_init(void* initrd_start);

#endif /* PERSPICUA_KERNEL_INITRD_H */
