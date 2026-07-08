/*
 * fat32.c - Implementation of the FAT32 filesystem driver.
 *
 * This module provides VFS-compatible operations for FAT32 filesystems,
 * including directory lookups, file reads, and directory listing.
 */

#include "fs/fat32.h"

#include "stdio.h"
#include "string.h"

#include "uapi/errors.h"

#include "core/lock.h"
#include "core/mutex.h"
#include "mm/slab.h"
#include "fs/pagecache.h"
#include "mm/pmm.h"
#include "mm/addr.h"

static struct fat32_fs current_fs;

/*
 * Coarse filesystem lock. FAT32 does blocking SD I/O, so this must be a
 * sleeping mutex (a spinlock can't be held across schedule()). It is recursive
 * because a read/write already holding it can re-enter write_page via the page
 * cache eviction path.
 */
static struct kmutex fat32_lock = KMUTEX_INIT;

/*
 * cluster_to_lba - Converts a FAT cluster number to a Logical Block Address.
 */
static uint32_t cluster_to_lba(uint32_t cluster)
{
    return current_fs.data_lba_start + (cluster - 2) * current_fs.sectors_per_cluster;
}

/*
 * get_next_cluster - Reads the FAT to find the next cluster in a chain.
 */
static uint32_t get_next_cluster(uint32_t cluster)
{
    uint32_t fat_sector = current_fs.fat_lba_start + (cluster / 128);
    uint32_t fat_offset = cluster % 128;
    uint32_t fat_buffer[128];

    if (current_fs.dev->read_blocks(current_fs.dev, &fat_buffer, fat_sector, 1) != 0) {
        return 0x0FFFFFFF;
    }

    return fat_buffer[fat_offset] & 0x0FFFFFFF;
}

/*
 * set_fat_entry - Writes a value to a FAT entry for a given cluster.
 *
 * Updates all FAT copies to maintain filesystem consistency.
 */
static int set_fat_entry(uint32_t cluster, uint32_t value)
{
    uint32_t fat_sector_offset = cluster / 128;
    uint32_t fat_offset = cluster % 128;
    uint32_t fat_buffer[128];

    for (uint32_t i = 0; i < current_fs.num_fats; i++) {
        uint32_t fat_sector =
            current_fs.fat_lba_start + (i * current_fs.sectors_per_fat) + fat_sector_offset;

        if (current_fs.dev->read_blocks(current_fs.dev, fat_buffer, fat_sector, 1) != 0) {
            return -PERS_ERR_IO_ERROR;
        }

        fat_buffer[fat_offset] = value & 0x0FFFFFFF;

        if (current_fs.dev->write_blocks(current_fs.dev, fat_buffer, fat_sector, 1) != 0) {
            return -PERS_ERR_IO_ERROR;
        }
    }

    return PERS_SUCCESS;
}

/*
 * allocate_cluster - Finds a free cluster in the FAT and marks it as end-of-chain.
 *
 * Returns the cluster number on success, or 0 on failure.
 */
static uint32_t allocate_cluster(void)
{
    uint32_t fat_buffer[128];
    uint32_t total_data_sectors =
        current_fs.dev->block_count - current_fs.data_lba_start + current_fs.partition_lba_start;
    uint32_t total_clusters = total_data_sectors / current_fs.sectors_per_cluster;

    for (uint32_t cluster = 2; cluster < total_clusters + 2; cluster++) {
        uint32_t fat_sector = current_fs.fat_lba_start + (cluster / 128);
        uint32_t fat_offset = cluster % 128;

        if (fat_offset == 0 || cluster == 2) {
            if (current_fs.dev->read_blocks(current_fs.dev, fat_buffer, fat_sector, 1) != 0) {
                return 0;
            }
        }

        uint32_t entry = fat_buffer[fat_offset] & 0x0FFFFFFF;
        if (entry == 0) {
            if (set_fat_entry(cluster, 0x0FFFFFFF) != PERS_SUCCESS) {
                return 0;
            }
            return cluster;
        }
    }

    return 0;
}

/*
 * extend_cluster_chain - Allocates a new cluster and links it to an existing chain.
 *
 * Returns the newly allocated cluster number, or 0 on failure.
 */
static uint32_t extend_cluster_chain(uint32_t last_cluster)
{
    uint32_t new_cluster = allocate_cluster();
    if (new_cluster == 0) {
        return 0;
    }

    if (last_cluster != 0) {
        if (set_fat_entry(last_cluster, new_cluster) != PERS_SUCCESS) {
            set_fat_entry(new_cluster, 0);
            return 0;
        }
    }

    return new_cluster;
}

/*
 * name_match - Compares a long filename with a short 8.3 FAT directory entry.
 */
static int name_match(const char *filename, struct fat32_dir_entry *entry)
{
    char name[8], ext[3];
    int i = 0, j = 0;

    for (int k = 0; k < 8; k++) {
        name[k] = ' ';
    }
    for (int k = 0; k < 3; k++) {
        ext[k] = ' ';
    }

    while (filename[i] != '.' && filename[i] != '\0' && j < 8) {
        char c = filename[i++];
        if (c >= 'a' && c <= 'z') {
            c -= 32;
        }
        name[j++] = c;
    }

    if (filename[i] == '.') {
        i++;
        j = 0;
        while (filename[i] != '\0' && j < 3) {
            char c = filename[i++];
            if (c >= 'a' && c <= 'z') {
                c -= 32;
            }
            ext[j++] = c;
        }
    }

    for (int k = 0; k < 8; k++) {
        if (name[k] != entry->name[k]) {
            return 0;
        }
    }
    for (int k = 0; k < 3; k++) {
        if (ext[k] != entry->ext[k]) {
            return 0;
        }
    }
    return 1;
}

static void extract_lfn_part(struct fat32_lfn_entry *lfn, char *name_buf)
{
    int offset = ((lfn->sequence & 0x3F) - 1) * 13;

    // name1 (5 chars)
    for (int i = 0; i < 5; i++)
        name_buf[offset + i] = (char)lfn->name1[i];
    // name2 (6 chars)
    for (int i = 0; i < 6; i++)
        name_buf[offset + 5 + i] = (char)lfn->name2[i];
    // name3 (2 chars)
    for (int i = 0; i < 2; i++)
        name_buf[offset + 11 + i] = (char)lfn->name3[i];
}

/*
 * fat32_update_dir_entry - Updates the directory entry for a vnode.
 *
 * Searches the parent directory for the matching entry and updates
 * the file size field. Called after write operations to persist metadata.
 */
static int fat32_update_dir_entry(struct vfs_vnode *node)
{
    if (!node->parent) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    uint32_t target_cluster = (uint32_t)(uintptr_t)node->internal_info;
    uint32_t cluster = (uint32_t)(uintptr_t)node->parent->internal_info;
    struct fat32_dir_entry dirs[16];

    while (cluster < 0x0FFFFFF8) {
        uint32_t lba = cluster_to_lba(cluster);
        for (int s = 0; s < (int)current_fs.sectors_per_cluster; s++) {
            if (current_fs.dev->read_blocks(current_fs.dev, dirs, lba + s, 1) != 0) {
                return -PERS_ERR_IO_ERROR;
            }

            for (int i = 0; i < 16; i++) {
                if (dirs[i].name[0] == 0x00) {
                    return -PERS_ERR_NOT_FOUND;
                }
                if (dirs[i].name[0] == 0xE5 || dirs[i].attributes == 0x0F) {
                    continue;
                }

                uint32_t entry_cluster =
                    ((uint32_t)dirs[i].cluster_high << 16) | dirs[i].cluster_low;
                int match = (entry_cluster == target_cluster)
                            || (entry_cluster == 0 && name_match(node->name, &dirs[i]));
                if (match) {
                    dirs[i].size = (uint32_t)node->file_size;
                    dirs[i].cluster_high = (uint16_t)(target_cluster >> 16);
                    dirs[i].cluster_low = (uint16_t)(target_cluster & 0xFFFF);
                    if (current_fs.dev->write_blocks(current_fs.dev, dirs, lba + s, 1) != 0) {
                        return -PERS_ERR_IO_ERROR;
                    }
                    return PERS_SUCCESS;
                }
            }
        }
        cluster = get_next_cluster(cluster);
    }

    return -PERS_ERR_NOT_FOUND;
}

static int fat32_read_page(struct vfs_vnode *node, size_t page_index, void *page_buffer)
{
    uint32_t start_offset = page_index * PAGE_SIZE;
    if (start_offset >= node->file_size)
        return 0;

    uint32_t cluster = (uint32_t)(uintptr_t)node->internal_info;
    uint32_t bytes_per_cluster = current_fs.sectors_per_cluster * 512;
    uint32_t clusters_to_skip = start_offset / bytes_per_cluster;

    for (uint32_t i = 0; i < clusters_to_skip; i++) {
        cluster = get_next_cluster(cluster);
        if (cluster >= 0x0FFFFFF8)
            return 0;
    }

    uint32_t bytes_read = 0;
    uint32_t to_read = PAGE_SIZE;
    if (start_offset + to_read > node->file_size) {
        to_read = node->file_size - start_offset;
    }

    memset(page_buffer, 0, PAGE_SIZE);

    uint8_t sector_buffer[512];
    uint32_t current_offset = start_offset;

    while (bytes_read < to_read) {
        uint32_t offset_in_cluster = current_offset % bytes_per_cluster;
        uint32_t sector_in_cluster = offset_in_cluster / 512;
        uint32_t offset_in_sector = offset_in_cluster % 512;

        uint32_t lba = cluster_to_lba(cluster) + sector_in_cluster;

        if (current_fs.dev->read_blocks(current_fs.dev, sector_buffer, lba, 1) != 0) {
            break;
        }

        uint32_t can_read = 512 - offset_in_sector;
        uint32_t remaining = to_read - bytes_read;
        uint32_t to_copy = (can_read < remaining) ? can_read : remaining;

        for (uint32_t i = 0; i < to_copy; i++) {
            ((uint8_t *)page_buffer)[bytes_read + i] = sector_buffer[offset_in_sector + i];
        }

        bytes_read += to_copy;
        current_offset += to_copy;

        if (current_offset % bytes_per_cluster == 0) {
            cluster = get_next_cluster(cluster);
            if (cluster >= 0x0FFFFFF8 && bytes_read < to_read) {
                break;
            }
        }
    }
    return bytes_read;
}

/*
 * fat32_write_page - Writes a 4KB page to the file's data clusters.
 *
 * Allocates new clusters as needed to accommodate the write position.
 */
static int fat32_write_page(struct vfs_vnode *node, size_t page_index, void *page_buffer,
                            size_t valid_bytes)
{
    uint32_t start_offset = page_index * PAGE_SIZE;
    uint32_t bytes_per_cluster = current_fs.sectors_per_cluster * 512;

    uint32_t cluster = (uint32_t)(uintptr_t)node->internal_info;
    uint32_t cluster_index = 0;
    uint32_t clusters_to_skip = start_offset / bytes_per_cluster;

    /* Handle empty file - allocate first cluster */
    if (cluster == 0) {
        cluster = allocate_cluster();
        if (cluster == 0) {
            return -PERS_ERR_NO_SPACE_LEFT;
        }
        node->internal_info = (void *)(uintptr_t)cluster;
    }

    /* Navigate to the starting cluster */
    while (cluster_index < clusters_to_skip) {
        uint32_t next = get_next_cluster(cluster);
        if (next >= 0x0FFFFFF8) {
            /* Need to extend the chain */
            next = extend_cluster_chain(cluster);
            if (next == 0) {
                return -PERS_ERR_NO_SPACE_LEFT;
            }
        }
        cluster = next;
        cluster_index++;
    }

    uint32_t bytes_written = 0;
    uint32_t current_offset = start_offset;
    uint8_t sector_buffer[512];

    while (bytes_written < valid_bytes) {
        uint32_t offset_in_cluster = current_offset % bytes_per_cluster;
        uint32_t sector_in_cluster = offset_in_cluster / 512;
        uint32_t offset_in_sector = offset_in_cluster % 512;

        uint32_t lba = cluster_to_lba(cluster) + sector_in_cluster;

        /* Read-modify-write for partial sector updates */
        if (offset_in_sector != 0 || (valid_bytes - bytes_written) < 512) {
            if (current_fs.dev->read_blocks(current_fs.dev, sector_buffer, lba, 1) != 0) {
                memset(sector_buffer, 0, 512);
            }
        }

        uint32_t can_write = 512 - offset_in_sector;
        uint32_t remaining = valid_bytes - bytes_written;
        uint32_t to_copy = (can_write < remaining) ? can_write : remaining;

        for (uint32_t i = 0; i < to_copy; i++) {
            sector_buffer[offset_in_sector + i] = ((uint8_t *)page_buffer)[bytes_written + i];
        }

        if (current_fs.dev->write_blocks(current_fs.dev, sector_buffer, lba, 1) != 0) {
            return bytes_written > 0 ? (int)bytes_written : -PERS_ERR_IO_ERROR;
        }

        bytes_written += to_copy;
        current_offset += to_copy;

        if (current_offset % bytes_per_cluster == 0 && bytes_written < valid_bytes) {
            uint32_t next = get_next_cluster(cluster);
            if (next >= 0x0FFFFFF8) {
                next = extend_cluster_chain(cluster);
                if (next == 0) {
                    return (int)bytes_written;
                }
            }
            cluster = next;
        }
    }

    return (int)bytes_written;
}

static int fat32_vfs_read(struct vfs_file *file, void *buffer, size_t size)
{
    if (!file || !file->node || !buffer) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    uint32_t file_size = (uint32_t)file->node->file_size;
    uint32_t offset = (uint32_t)file->offset;

    if (offset >= file_size)
        return 0;
    if (offset + size > file_size)
        size = file_size - offset;

    uint32_t bytes_read = 0;
    uint8_t *out_buf = (uint8_t *)buffer;

    while (bytes_read < size) {
        size_t current_offset = offset + bytes_read;
        size_t page_index = current_offset / PAGE_SIZE;
        size_t offset_in_page = current_offset % PAGE_SIZE;

        size_t to_copy = PAGE_SIZE - offset_in_page;
        if (to_copy > size - bytes_read) {
            to_copy = size - bytes_read;
        }

        void *page_data = pagecache_get_page(file->node, page_index);
        if (!page_data) {
            page_data = pmm_alloc_pages(1);
            if (!page_data) {
                return bytes_read > 0 ? (int)bytes_read : -PERS_ERR_OUT_OF_MEMORY;
            }

            fat32_read_page(file->node, page_index, page_data);

            if (pagecache_add_page(file->node, page_index, page_data) != PERS_SUCCESS) {
                pmm_free_pages(page_data, 1);
                page_data = pagecache_get_page(file->node, page_index);
            }
        }

        if (page_data) {
            for (size_t i = 0; i < to_copy; i++) {
                out_buf[bytes_read + i] = ((uint8_t *)page_data)[offset_in_page + i];
            }
        } else {
            break;
        }

        bytes_read += to_copy;
    }

    file->offset += bytes_read;
    return (int)bytes_read;
}

static int fat32_vfs_write(struct vfs_file *file, const void *buffer, size_t size)
{
    if (!file || !file->node || !buffer) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    if (file->node->type == VFS_VNODE_TYPE_DIR) {
        return -PERS_ERR_IS_A_DIRECTORY;
    }

    uint32_t offset = (uint32_t)file->offset;
    uint32_t bytes_written = 0;
    const uint8_t *in_buf = (const uint8_t *)buffer;

    while (bytes_written < size) {
        size_t current_offset = offset + bytes_written;
        size_t page_index = current_offset / PAGE_SIZE;
        size_t offset_in_page = current_offset % PAGE_SIZE;

        size_t to_copy = PAGE_SIZE - offset_in_page;
        if (to_copy > size - bytes_written) {
            to_copy = size - bytes_written;
        }

        void *page_data = pagecache_get_page(file->node, page_index);
        if (!page_data) {
            page_data = pmm_alloc_pages(1);
            if (!page_data) {
                return bytes_written > 0 ? (int)bytes_written : -PERS_ERR_OUT_OF_MEMORY;
            }
            memset(page_data, 0, PAGE_SIZE);

            /* Read existing data if we're writing within or extending the file */
            if (page_index * PAGE_SIZE < (size_t)file->node->file_size) {
                fat32_read_page(file->node, page_index, page_data);
            }

            if (pagecache_add_page(file->node, page_index, page_data) != PERS_SUCCESS) {
                pmm_free_pages(page_data, 1);
                page_data = pagecache_get_page(file->node, page_index);
                if (!page_data) {
                    return bytes_written > 0 ? (int)bytes_written : -PERS_ERR_OUT_OF_MEMORY;
                }
            }
        }

        /* Copy data into the page */
        for (size_t i = 0; i < to_copy; i++) {
            ((uint8_t *)page_data)[offset_in_page + i] = in_buf[bytes_written + i];
        }

        pagecache_mark_dirty(file->node, page_index);

        /* Write through to disk immediately */
        size_t page_end = offset_in_page + to_copy;
        size_t valid_in_page = page_end;
        if ((page_index + 1) * PAGE_SIZE <= (size_t)file->node->file_size) {
            valid_in_page = PAGE_SIZE;
        }

        int write_result = fat32_write_page(file->node, page_index, page_data, valid_in_page);
        if (write_result < 0) {
            return bytes_written > 0 ? (int)bytes_written : write_result;
        }

        pagecache_clear_dirty(file->node, page_index);

        bytes_written += to_copy;
    }

    file->offset += bytes_written;

    /* Update file size and directory entry if we extended the file */
    if (file->offset > file->node->file_size) {
        file->node->file_size = file->offset;
        fat32_update_dir_entry(file->node);
    }

    return (int)bytes_written;
}

static struct vfs_vnode_ops fat32_vnode_ops;

static void name_to_83(const char *filename, uint8_t out_name[8], uint8_t out_ext[3])
{
    for (int k = 0; k < 8; k++)
        out_name[k] = ' ';
    for (int k = 0; k < 3; k++)
        out_ext[k] = ' ';

    int i = 0, j = 0;
    while (filename[i] != '.' && filename[i] != '\0' && j < 8) {
        char c = filename[i++];
        if (c >= 'a' && c <= 'z')
            c -= 32;
        out_name[j++] = c;
    }
    while (filename[i] != '.' && filename[i] != '\0')
        i++;
    if (filename[i] == '.') {
        i++;
        j = 0;
        while (filename[i] != '\0' && j < 3) {
            char c = filename[i++];
            if (c >= 'a' && c <= 'z')
                c -= 32;
            out_ext[j++] = c;
        }
    }
}

static int fat32_free_cluster_chain(uint32_t start_cluster)
{
    uint32_t current = start_cluster;
    while (current < 0x0FFFFFF8 && current >= 2) {
        uint32_t next = get_next_cluster(current);
        if (set_fat_entry(current, 0) != PERS_SUCCESS) {
            return -PERS_ERR_IO_ERROR;
        }
        current = next;
    }
    return PERS_SUCCESS;
}

static int fat32_write_entry_to_parent(uint32_t parent_cluster, struct fat32_dir_entry *new_entry)
{
    struct fat32_dir_entry dirs[16];
    uint32_t cluster = parent_cluster;

    while (cluster < 0x0FFFFFF8) {
        uint32_t lba = cluster_to_lba(cluster);
        for (int s = 0; s < (int)current_fs.sectors_per_cluster; s++) {
            if (current_fs.dev->read_blocks(current_fs.dev, &dirs, lba + s, 1) != 0) {
                return -PERS_ERR_IO_ERROR;
            }
            for (int i = 0; i < 16; i++) {
                if (dirs[i].name[0] == 0x00 || dirs[i].name[0] == 0xE5) {
                    dirs[i] = *new_entry;
                    if (current_fs.dev->write_blocks(current_fs.dev, &dirs, lba + s, 1) != 0) {
                        return -PERS_ERR_IO_ERROR;
                    }
                    return PERS_SUCCESS;
                }
            }
        }
        uint32_t next = get_next_cluster(cluster);
        if (next >= 0x0FFFFFF8) {
            next = extend_cluster_chain(cluster);
            if (next == 0)
                return -PERS_ERR_OUT_OF_MEMORY;
        }
        cluster = next;
    }
    return -PERS_ERR_OUT_OF_MEMORY;
}

static struct vfs_vnode *fat32_vfs_lookup(struct vfs_vnode *dir, const char *filename)
{
    uint32_t cluster = (uint32_t)(uintptr_t)dir->internal_info;
    struct fat32_dir_entry dirs[16];

    char lfn_name[256];
    memset(lfn_name, 0, 256);
    int has_lfn = 0;

    while (cluster < 0x0FFFFFF8) {
        uint32_t lba = cluster_to_lba(cluster);
        for (int s = 0; s < (int)current_fs.sectors_per_cluster; s++) {
            if (current_fs.dev->read_blocks(current_fs.dev, &dirs, lba + s, 1) != 0) {
                return NULL;
            }
            for (int i = 0; i < 16; i++) {
                if (dirs[i].name[0] == 0x00) {
                    return NULL;
                }
                if (dirs[i].name[0] == 0xE5) {
                    goto reset_lfn;
                }

                if (dirs[i].attributes == 0x0F) {
                    struct fat32_lfn_entry *lfn = (struct fat32_lfn_entry *)&dirs[i];
                    extract_lfn_part(lfn, lfn_name);
                    has_lfn = 1;
                    continue;
                }
                if ((has_lfn && strcmp(filename, lfn_name) == 0)
                    || name_match(filename, &dirs[i])) {
                    struct vfs_vnode *node =
                        (struct vfs_vnode *)slab_alloc(sizeof(struct vfs_vnode));
                    if (!node) {
                        return NULL;
                    }
                    memset(node, 0, sizeof(struct vfs_vnode));
                    node->type =
                        (dirs[i].attributes & 0x10) ? VFS_VNODE_TYPE_DIR : VFS_VNODE_TYPE_REGULAR;
                    node->ops = &fat32_vnode_ops;
                    node->internal_info =
                        (void *)(uintptr_t)((dirs[i].cluster_high << 16) | dirs[i].cluster_low);
                    node->file_size = dirs[i].size;
                    node->parent = dir;
                    atomic_inc(&dir->refcount);
                    node->refcount.counter = 1;
                    return node;
                }
reset_lfn:
                has_lfn = 0;
                memset(lfn_name, 0, 256);
            }
        }
        cluster = get_next_cluster(cluster);
    }
    return NULL;
}

static int fat32_vfs_readdir(struct vfs_file *file, void *buffer, size_t count)
{
    if (file->node->type != VFS_VNODE_TYPE_DIR) {
        return -PERS_ERR_NOT_A_DIRECTORY;
    }

    struct vfs_dirent *dirent_buf = (struct vfs_dirent *)buffer;
    size_t max_entries = count / sizeof(struct vfs_dirent);
    int entries_read = 0;

    uint32_t cluster = (uint32_t)(uintptr_t)file->node->internal_info;
    uint32_t bytes_per_cluster = current_fs.sectors_per_cluster * 512;

    uint32_t clusters_to_skip = (uint32_t)(file->offset / bytes_per_cluster);
    for (uint32_t i = 0; i < clusters_to_skip; i++) {
        cluster = get_next_cluster(cluster);
        if (cluster >= 0x0FFFFFF8) {
            return 0;
        }
    }

    char lfn_name[256];
    memset(lfn_name, 0, 256);
    int has_lfn = 0;

    struct fat32_dir_entry dirs[16];
    while (cluster < 0x0FFFFFF8 && entries_read < (int)max_entries) {
        uint32_t offset_in_cluster = (uint32_t)(file->offset % bytes_per_cluster);
        uint32_t start_sector = offset_in_cluster / 512;

        for (uint32_t s = start_sector;
             s < current_fs.sectors_per_cluster && entries_read < (int)max_entries; s++) {
            uint32_t lba = cluster_to_lba(cluster) + s;
            if (current_fs.dev->read_blocks(current_fs.dev, &dirs, lba, 1) != 0) {
                return -PERS_ERR_IO_ERROR;
            }

            uint32_t start_entry = (uint32_t)((file->offset % 512) / 32);
            for (int i = (int)start_entry; i < 16 && entries_read < (int)max_entries; i++) {
                file->offset += 32;

                if (dirs[i].name[0] == 0x00)
                    return entries_read;

                if (dirs[i].name[0] == 0xE5) {
                    has_lfn = 0;
                    continue;
                }
                if (dirs[i].attributes == 0x0F) {
                    struct fat32_lfn_entry *lfn = (struct fat32_lfn_entry *)&dirs[i];
                    extract_lfn_part(lfn, lfn_name);
                    has_lfn = 1;
                    continue;
                }
                /* Skip . and .. entries from the FAT filesystem itself */
                if (dirs[i].name[0] == '.') {
                    continue;
                }

                struct vfs_dirent *dirent = &dirent_buf[entries_read];
                if (has_lfn) {
                    strncpy(dirent->name, lfn_name, 255);
                    dirent->name[255] = '\0';
                } else {
                    int idx = 0;
                    for (int k = 0; k < 8; k++) {
                        uint8_t c = dirs[i].name[k];
                        if (c == ' ' || c == 0) {
                            continue;
                        }
                        if (c >= 'A' && c <= 'Z') {
                            c = c - 'A' + 'a';
                        }
                        dirent->name[idx++] = (char)c;
                    }
                    if (dirs[i].ext[0] != ' ' && dirs[i].ext[0] != 0) {
                        dirent->name[idx++] = '.';
                        for (int k = 0; k < 3; k++) {
                            uint8_t c = dirs[i].ext[k];
                            if (c == ' ' || c == 0) {
                                continue;
                            }
                            if (c >= 'A' && c <= 'Z') {
                                c = c - 'A' + 'a';
                            }
                            dirent->name[idx++] = (char)c;
                        }
                    }
                    dirent->name[idx] = '\0';
                }

                has_lfn = 0;
                memset(lfn_name, 0, 256);
                dirent->ino = (uint32_t)((dirs[i].cluster_high << 16) | dirs[i].cluster_low);
                entries_read++;
            }
        }
        cluster = get_next_cluster(cluster);
    }

    return entries_read;
}

static int fat32_unlink(struct vfs_vnode *parent, const char *name)
{
    uint32_t cluster = (uint32_t)(uintptr_t)parent->internal_info;
    struct fat32_dir_entry dirs[16];
    char lfn_name[256];
    memset(lfn_name, 0, 256);
    int has_lfn = 0;

    while (cluster < 0x0FFFFFF8) {
        uint32_t lba = cluster_to_lba(cluster);
        for (int s = 0; s < (int)current_fs.sectors_per_cluster; s++) {
            if (current_fs.dev->read_blocks(current_fs.dev, &dirs, lba + s, 1) != 0) {
                return -PERS_ERR_IO_ERROR;
            }
            for (int i = 0; i < 16; i++) {
                if (dirs[i].name[0] == 0x00)
                    return -PERS_ERR_NOT_FOUND;
                if (dirs[i].name[0] == 0xE5) {
                    has_lfn = 0;
                    memset(lfn_name, 0, 256);
                    continue;
                }

                if (dirs[i].attributes == 0x0F) {
                    struct fat32_lfn_entry *lfn = (struct fat32_lfn_entry *)&dirs[i];
                    extract_lfn_part(lfn, lfn_name);
                    has_lfn = 1;
                    continue;
                }
                if ((has_lfn && strcmp(name, lfn_name) == 0) || name_match(name, &dirs[i])) {
                    if (dirs[i].attributes & 0x10) {
                        return -PERS_ERR_IS_A_DIRECTORY;
                    }
                    uint32_t target_cluster = (dirs[i].cluster_high << 16) | dirs[i].cluster_low;
                    dirs[i].name[0] = 0xE5;
                    if (current_fs.dev->write_blocks(current_fs.dev, &dirs, lba + s, 1) != 0) {
                        return -PERS_ERR_IO_ERROR;
                    }
                    if (target_cluster >= 2) {
                        fat32_free_cluster_chain(target_cluster);
                    }
                    return PERS_SUCCESS;
                }
                has_lfn = 0;
                memset(lfn_name, 0, 256);
            }
        }
        cluster = get_next_cluster(cluster);
    }
    return -PERS_ERR_NOT_FOUND;
}

static int fat32_rmdir(struct vfs_vnode *parent, const char *name)
{
    uint32_t cluster = (uint32_t)(uintptr_t)parent->internal_info;
    struct fat32_dir_entry dirs[16];
    char lfn_name[256];
    memset(lfn_name, 0, 256);
    int has_lfn = 0;

    while (cluster < 0x0FFFFFF8) {
        uint32_t lba = cluster_to_lba(cluster);
        for (int s = 0; s < (int)current_fs.sectors_per_cluster; s++) {
            if (current_fs.dev->read_blocks(current_fs.dev, &dirs, lba + s, 1) != 0) {
                return -PERS_ERR_IO_ERROR;
            }
            for (int i = 0; i < 16; i++) {
                if (dirs[i].name[0] == 0x00)
                    return -PERS_ERR_NOT_FOUND;
                if (dirs[i].name[0] == 0xE5) {
                    has_lfn = 0;
                    memset(lfn_name, 0, 256);
                    continue;
                }

                if (dirs[i].attributes == 0x0F) {
                    struct fat32_lfn_entry *lfn = (struct fat32_lfn_entry *)&dirs[i];
                    extract_lfn_part(lfn, lfn_name);
                    has_lfn = 1;
                    continue;
                }
                if ((has_lfn && strcmp(name, lfn_name) == 0) || name_match(name, &dirs[i])) {
                    if (!(dirs[i].attributes & 0x10)) {
                        return -PERS_ERR_NOT_A_DIRECTORY;
                    }
                    uint32_t target_cluster = (dirs[i].cluster_high << 16) | dirs[i].cluster_low;

                    uint32_t scan_cluster = target_cluster;
                    struct fat32_dir_entry scan_dirs[16];
                    while (scan_cluster < 0x0FFFFFF8 && scan_cluster >= 2) {
                        uint32_t scan_lba = cluster_to_lba(scan_cluster);
                        for (int scan_s = 0; scan_s < (int)current_fs.sectors_per_cluster;
                             scan_s++) {
                            if (current_fs.dev->read_blocks(current_fs.dev, &scan_dirs,
                                                            scan_lba + scan_s, 1)
                                != 0) {
                                return -PERS_ERR_IO_ERROR;
                            }
                            for (int scan_i = 0; scan_i < 16; scan_i++) {
                                if (scan_dirs[scan_i].name[0] == 0x00)
                                    break;
                                if (scan_dirs[scan_i].name[0] != 0xE5
                                    && scan_dirs[scan_i].name[0] != '.') {
                                    return -PERS_ERR_DIR_NOT_EMPTY;
                                }
                            }
                        }
                        scan_cluster = get_next_cluster(scan_cluster);
                    }

                    dirs[i].name[0] = 0xE5;
                    if (current_fs.dev->write_blocks(current_fs.dev, &dirs, lba + s, 1) != 0) {
                        return -PERS_ERR_IO_ERROR;
                    }
                    if (target_cluster >= 2) {
                        fat32_free_cluster_chain(target_cluster);
                    }
                    return PERS_SUCCESS;
                }
                has_lfn = 0;
                memset(lfn_name, 0, 256);
            }
        }
        cluster = get_next_cluster(cluster);
    }
    return -PERS_ERR_NOT_FOUND;
}

static int fat32_mkdir(struct vfs_vnode *parent, const char *name)
{
    if (strlen(name) > 255)
        return -PERS_ERR_INVALID_ARGUMENT;

    struct vfs_vnode *existing = fat32_vfs_lookup(parent, name);
    if (existing) {
        slab_free(existing);
        return -PERS_ERR_ALREADY_EXISTS;
    }

    uint32_t new_cluster = allocate_cluster();
    if (new_cluster == 0)
        return -PERS_ERR_OUT_OF_MEMORY;

    uint8_t zero_sector[512];
    memset(zero_sector, 0, 512);
    for (uint32_t s = 0; s < current_fs.sectors_per_cluster; s++) {
        if (current_fs.dev->write_blocks(current_fs.dev, zero_sector,
                                         cluster_to_lba(new_cluster) + s, 1)
            != 0) {
            return -PERS_ERR_IO_ERROR;
        }
    }

    struct fat32_dir_entry sector[16];
    memset(sector, 0, sizeof(sector));

    memset(sector[0].name, ' ', 8);
    sector[0].name[0] = '.';
    memset(sector[0].ext, ' ', 3);
    sector[0].attributes = 0x10;
    sector[0].cluster_high = new_cluster >> 16;
    sector[0].cluster_low = new_cluster & 0xFFFF;

    memset(sector[1].name, ' ', 8);
    sector[1].name[0] = '.';
    sector[1].name[1] = '.';
    memset(sector[1].ext, ' ', 3);
    sector[1].attributes = 0x10;
    uint32_t parent_cluster = (uint32_t)(uintptr_t)parent->internal_info;
    sector[1].cluster_high = parent_cluster >> 16;
    sector[1].cluster_low = parent_cluster & 0xFFFF;

    if (current_fs.dev->write_blocks(current_fs.dev, sector, cluster_to_lba(new_cluster), 1) != 0) {
        return -PERS_ERR_IO_ERROR;
    }

    struct fat32_dir_entry new_entry;
    memset(&new_entry, 0, sizeof(new_entry));
    name_to_83(name, new_entry.name, new_entry.ext);
    new_entry.attributes = 0x10;
    new_entry.cluster_high = new_cluster >> 16;
    new_entry.cluster_low = new_cluster & 0xFFFF;
    new_entry.size = 0;

    return fat32_write_entry_to_parent(parent_cluster, &new_entry);
}

static int fat32_rename(struct vfs_vnode *old_parent, const char *old_name,
                        struct vfs_vnode *new_parent, const char *new_name)
{
    uint32_t old_cluster = (uint32_t)(uintptr_t)old_parent->internal_info;
    struct fat32_dir_entry dirs[16];
    char lfn_name[256];
    memset(lfn_name, 0, 256);
    int has_lfn = 0;
    struct fat32_dir_entry target_entry;
    int found = 0;
    uint32_t found_lba = 0;
    int found_s = 0;
    int found_i = 0;

    while (old_cluster < 0x0FFFFFF8 && !found) {
        uint32_t lba = cluster_to_lba(old_cluster);
        for (int s = 0; s < (int)current_fs.sectors_per_cluster; s++) {
            if (current_fs.dev->read_blocks(current_fs.dev, &dirs, lba + s, 1) != 0) {
                return -PERS_ERR_IO_ERROR;
            }
            for (int i = 0; i < 16; i++) {
                if (dirs[i].name[0] == 0x00)
                    break;
                if (dirs[i].name[0] == 0xE5) {
                    has_lfn = 0;
                    memset(lfn_name, 0, 256);
                    continue;
                }

                if (dirs[i].attributes == 0x0F) {
                    struct fat32_lfn_entry *lfn = (struct fat32_lfn_entry *)&dirs[i];
                    extract_lfn_part(lfn, lfn_name);
                    has_lfn = 1;
                    continue;
                }
                if ((has_lfn && strcmp(old_name, lfn_name) == 0)
                    || name_match(old_name, &dirs[i])) {
                    target_entry = dirs[i];
                    found = 1;
                    found_lba = lba;
                    found_s = s;
                    found_i = i;
                    break;
                }
                has_lfn = 0;
                memset(lfn_name, 0, 256);
            }
            if (found)
                break;
        }
        if (!found)
            old_cluster = get_next_cluster(old_cluster);
    }
    if (!found)
        return -PERS_ERR_NOT_FOUND;

    struct fat32_dir_entry new_entry = target_entry;
    name_to_83(new_name, new_entry.name, new_entry.ext);

    uint32_t new_parent_cluster = (uint32_t)(uintptr_t)new_parent->internal_info;
    int res = fat32_write_entry_to_parent(new_parent_cluster, &new_entry);
    if (res != PERS_SUCCESS)
        return res;

    if (current_fs.dev->read_blocks(current_fs.dev, &dirs, found_lba + found_s, 1) != 0) {
        return -PERS_ERR_IO_ERROR;
    }
    dirs[found_i].name[0] = 0xE5;
    if (current_fs.dev->write_blocks(current_fs.dev, &dirs, found_lba + found_s, 1) != 0) {
        return -PERS_ERR_IO_ERROR;
    }

    return PERS_SUCCESS;
}

static int fat32_create(struct vfs_vnode *parent, const char *name)
{
    struct fat32_dir_entry new_entry;
    memset(&new_entry, 0, sizeof(new_entry));
    name_to_83(name, new_entry.name, new_entry.ext);
    new_entry.attributes = 0x20; /* archive = regular file */
    new_entry.cluster_high = 0;
    new_entry.cluster_low = 0;
    new_entry.size = 0;

    uint32_t parent_cluster = (uint32_t)(uintptr_t)parent->internal_info;
    return fat32_write_entry_to_parent(parent_cluster, &new_entry);
}

/*
 * Registered vnode ops. Each is a thin wrapper that serializes a FAT32 entry
 * point under fat32_lock; the internal helpers above assume the lock is held.
 * The lock is recursive so page-cache eviction can re-enter write_page while a
 * read/write already holds it.
 */
static int fat32_op_read(struct vfs_file *file, void *buffer, size_t size)
{
    kmutex_lock(&fat32_lock);
    int r = fat32_vfs_read(file, buffer, size);
    kmutex_unlock(&fat32_lock);
    return r;
}

static int fat32_op_write(struct vfs_file *file, const void *buffer, size_t size)
{
    kmutex_lock(&fat32_lock);
    int r = fat32_vfs_write(file, buffer, size);
    kmutex_unlock(&fat32_lock);
    return r;
}

static struct vfs_vnode *fat32_op_lookup(struct vfs_vnode *dir, const char *name)
{
    kmutex_lock(&fat32_lock);
    struct vfs_vnode *n = fat32_vfs_lookup(dir, name);
    kmutex_unlock(&fat32_lock);
    return n;
}

static int fat32_op_readdir(struct vfs_file *file, void *buffer, size_t count)
{
    kmutex_lock(&fat32_lock);
    int r = fat32_vfs_readdir(file, buffer, count);
    kmutex_unlock(&fat32_lock);
    return r;
}

static int fat32_op_write_page(struct vfs_vnode *node, size_t page_index, void *page_buffer,
                               size_t valid_bytes)
{
    kmutex_lock(&fat32_lock);
    int r = fat32_write_page(node, page_index, page_buffer, valid_bytes);
    kmutex_unlock(&fat32_lock);
    return r;
}

static int fat32_op_mkdir(struct vfs_vnode *parent, const char *name)
{
    kmutex_lock(&fat32_lock);
    int r = fat32_mkdir(parent, name);
    kmutex_unlock(&fat32_lock);
    return r;
}

static int fat32_op_create(struct vfs_vnode *parent, const char *name)
{
    kmutex_lock(&fat32_lock);
    int r = fat32_create(parent, name);
    kmutex_unlock(&fat32_lock);
    return r;
}

static int fat32_op_rmdir(struct vfs_vnode *parent, const char *name)
{
    kmutex_lock(&fat32_lock);
    int r = fat32_rmdir(parent, name);
    kmutex_unlock(&fat32_lock);
    return r;
}

static int fat32_op_unlink(struct vfs_vnode *parent, const char *name)
{
    kmutex_lock(&fat32_lock);
    int r = fat32_unlink(parent, name);
    kmutex_unlock(&fat32_lock);
    return r;
}

static int fat32_op_rename(struct vfs_vnode *old_parent, const char *old_name,
                           struct vfs_vnode *new_parent, const char *new_name)
{
    kmutex_lock(&fat32_lock);
    int r = fat32_rename(old_parent, old_name, new_parent, new_name);
    kmutex_unlock(&fat32_lock);
    return r;
}

static struct vfs_vnode_ops fat32_vnode_ops = {
    .read = fat32_op_read,
    .write = fat32_op_write,
    .lookup = fat32_op_lookup,
    .readdir = fat32_op_readdir,
    .write_page = fat32_op_write_page,
    .mkdir = fat32_op_mkdir,
    .create = fat32_op_create,
    .rmdir = fat32_op_rmdir,
    .unlink = fat32_op_unlink,
    .rename = fat32_op_rename,
};

/*
 * fat32_get_root_node - Returns the root directory vnode.
 */
struct vfs_vnode *fat32_get_root_node(void)
{
    struct vfs_vnode *node = slab_alloc(sizeof(struct vfs_vnode));
    if (!node) {
        return NULL;
    }
    node->type = VFS_VNODE_TYPE_DIR;
    node->ops = &fat32_vnode_ops;
    node->internal_info = (void *)(uintptr_t)current_fs.root_cluster;
    node->file_size = 0;
    return node;
}

/*
 * fat32_init - Validates the MBR/BPB and populates the global filesystem state.
 */
int fat32_init(const char *device_name)
{
    struct block_device *dev = block_device_lookup(device_name);
    if (!dev) {
        return -PERS_ERR_NOT_FOUND;
    }

    uint8_t sector0[512];
    if (dev->read_blocks(dev, sector0, 0, 1) != 0) {
        return -PERS_ERR_IO_ERROR;
    }

    uint16_t sig = *(uint16_t *)(sector0 + 510);
    if (sig != 0xAA55) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    if (sector0[0] == 0xEB || sector0[0] == 0xE9) {
        current_fs.dev = dev;
        current_fs.partition_lba_start = 0;
    } else {
        struct mbr *mbr = (struct mbr *)sector0;
        int found = 0;
        for (size_t i = 0; i < 4 && !found; i++) {
            if (mbr->partitions[i].type == 0x0B || mbr->partitions[i].type == 0x0C) {
                current_fs.dev = dev;
                current_fs.partition_lba_start = mbr->partitions[i].lba_start;
                found = 1;
            }
        }
        if (!found) {
            return -PERS_ERR_NOT_FOUND;
        }
    }

    struct fat32_bpb bpb;
    if (dev->read_blocks(dev, &bpb, current_fs.partition_lba_start, 1) != 0) {
        return -PERS_ERR_IO_ERROR;
    }

    current_fs.bytes_per_sector = bpb.bytes_per_sector;
    current_fs.reserved_sectors = bpb.reserved_sectors;
    current_fs.sectors_per_cluster = bpb.sectors_per_cluster;
    current_fs.root_cluster = bpb.root_cluster;
    current_fs.sectors_per_fat = bpb.sectors_per_fat_32;
    current_fs.num_fats = bpb.num_fats;
    current_fs.fat_lba_start = current_fs.partition_lba_start + current_fs.reserved_sectors;
    current_fs.data_lba_start =
        current_fs.fat_lba_start + (current_fs.num_fats * current_fs.sectors_per_fat);

    pr_info("fat32: partition at LBA %u\n", current_fs.partition_lba_start);
    return PERS_SUCCESS;
}
