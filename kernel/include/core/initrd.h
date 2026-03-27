/*
 * initrd.h - Public API for the Initial RAM Disk (InitRD) parser.
 *
 * Defines the CPIO archive structures and entry point for registering
 * the boot-time filesystem.
 */

#ifndef PERSPICUA_CORE_INITRD_H
#define PERSPICUA_CORE_INITRD_H

#include "types.h"

/* --- Data Structures --- */

/*
 * struct cpio_newc_header - Standard header for the CPIO 'newc' format.
 *
 * All fields are fixed-width hexadecimal ASCII strings. This format is
 * used for its portability and simplicity during early boot.
 */
struct cpio_newc_header {
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

/* --- Function Prototypes --- */

/*
 * initrd_init - Parses the CPIO archive and registers files into RAMFS.
 */
void initrd_init(void *initrd_start);

#endif /* PERSPICUA_CORE_INITRD_H */
