/*
 * fat32.c - Implementation of the FAT32 filesystem driver.
 */

#include "fs/fat32.h"

#include "fs/vfs.h"
#include "stdio.h"
#include "string.h"

#include "types.h"
#include "uapi/errors.h"

#include "core/lock.h"
#include "core/mutex.h"
#include "mm/slab.h"
#include "fs/pagecache.h"
#include "mm/pmm.h"
#include "mm/addr.h"
#include <stdint.h>
#include <stdbool.h>
/*
 * A FAT32 long name is at most 255 characters, carried 13 at a time by up to
 * 20 chained LFN entries. The sequence number of an entry indexes the write, so
 * these bounds are load-bearing: they come off the disk and cannot be trusted.
 */
#define FAT32_LFN_CHARS_PER_ENTRY 13
#define FAT32_LFN_MAX_SEQ         20
#define FAT32_LFN_BUF_SIZE        256

static struct fat32_fs current_fs;

/*
 * Coarse filesystem lock. FAT32 does blocking SD I/O, so this must be a
 * sleeping mutex (a spinlock can't be held across schedule()). It is recursive
 * because a read/write already holding it can re-enter write_page via the page
 * cache eviction path.
 */
static struct kmutex fat32_lock = KMUTEX_INIT;

// Every read and write path in this driver assumes 512-byte sectors.
#define FAT32_SECTOR_SIZE 512

// Highest cluster number FAT32 can address; above this are reserved markers.
#define FAT32_CLUSTER_MAX 0x0FFFFFF6U

/*
 * fat32_dir_walk - Shared cluster -> sector -> 16-entry directory scan.
 *
 * Every directory operation (lookup, unlink, rmdir, rename, ...) needs this
 * same walk; only what happens on a given entry differs. fat32_dir_walk does
 * the walking and hands each real entry (a live entry, a deleted entry, or
 * the end-of-directory marker) to `cb`, along with whatever long name was
 * assembled from any LFN fragments immediately before it. LFN-continuation
 * entries themselves are consumed internally and never shown to `cb`.
 *
 * Declared up here because fat32_update_dir_entry (below) needs it before its
 * definition further down the file, next to its first caller.
 */
enum fat32_walk_action {
    FAT32_WALK_CONTINUE = 0, // not it -- keep scanning
    FAT32_WALK_STOP = 1,     // found what I wanted, stop the whole walk
    FAT32_WALK_ERROR = 2,    // cb hit its own failure; error is left in *out_err
};

typedef enum fat32_walk_action (*fat32_dir_walk_cb)(struct fat32_dir_entry *entry,
                                                    const char *lfn_name, int has_lfn, uint32_t lba,
                                                    int sector_index, int entry_index, void *ctx,
                                                    int *out_err);

int fat32_dir_walk(uint32_t parent_cluster, bool grow_chain, fat32_dir_walk_cb cb, void *ctx);

/*
 * cluster_valid - True for a cluster the data area can actually address.
 *
 * Cluster numbers come from directory entries and the FAT itself, so they are
 * untrusted input however deep in the driver they surface.
 */
static int cluster_valid(uint32_t cluster)
{
    return cluster >= 2 && cluster <= current_fs.max_cluster;
}

/*
 * cluster_to_lba - Converts a FAT cluster number to a Logical Block Address.
 *
 * Callers add a sector offset to the result, so the out-of-range sentinel has
 * to stay huge after that addition: 0xFFFFFFFF + 1 wraps to zero, which is the
 * MBR, and one caller writes there. sectors_per_cluster is capped at 128 when
 * the volume is mounted, so this value cannot wrap.
 */
#define FAT32_LBA_INVALID 0xFFF00000U

static uint32_t cluster_to_lba(uint32_t cluster)
{
    if (!cluster_valid(cluster)) {
        return FAT32_LBA_INVALID;
    }
    return current_fs.data_lba_start + (cluster - 2) * current_fs.sectors_per_cluster;
}

static uint32_t get_next_cluster(uint32_t cluster)
{
    // Terminate the chain rather than index the FAT with a corrupt value.
    if (!cluster_valid(cluster)) {
        return 0x0FFFFFFF;
    }

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

    // Scan clusters within the validated mount-time bound.
    for (uint32_t cluster = 2; cluster <= current_fs.max_cluster; cluster++) {
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

    /*
     * A basename past the eighth character is dropped, so skip the overflow to
     * reach the extension. name_to_83 does the same when it builds the entry,
     * and a lookup that stops short searches for a blank extension instead:
     * the file it just created is then unfindable, and the next create appends
     * a second entry for the same name.
     */
    while (filename[i] != '.' && filename[i] != '\0') {
        i++;
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

/*
 * extract_lfn_part - Places one 13-character fragment of a long name.
 *
 * The destination index is derived from the entry's on-disk sequence number, so
 * it is range-checked before any write: an out-of-spec sequence would otherwise
 * scatter the fragment far outside name_buf. Returns 0 on success, or -1 if the
 * entry is malformed and the caller should discard the name assembled so far.
 */
static int extract_lfn_part(const struct fat32_lfn_entry *lfn, char *name_buf, size_t buf_len)
{
    unsigned int seq = lfn->sequence & 0x3F;
    if (seq < 1 || seq > FAT32_LFN_MAX_SEQ) {
        return -1;
    }

    size_t limit = buf_len - 1; // Reserve the terminator.
    size_t pos = (size_t)(seq - 1) * FAT32_LFN_CHARS_PER_ENTRY;
    if (pos >= limit) {
        return -1;
    }

    /*
     * The final fragment of a maximum-length name runs a few characters past
     * the last usable byte; copy what fits rather than rejecting a valid name.
     * Members are read individually because the entry struct is packed.
     */
    for (int i = 0; i < 5 && pos < limit; i++) {
        name_buf[pos++] = (char)lfn->name1[i];
    }
    for (int i = 0; i < 6 && pos < limit; i++) {
        name_buf[pos++] = (char)lfn->name2[i];
    }
    for (int i = 0; i < 2 && pos < limit; i++) {
        name_buf[pos++] = (char)lfn->name3[i];
    }

    return 0;
}

#ifdef CONFIG_TESTS
int fat32_test_extract_lfn_part(const struct fat32_lfn_entry *lfn, char *name_buf, size_t buf_len)
{
    return extract_lfn_part(lfn, name_buf, buf_len);
}
#endif

/*
 * fat32_update_dir_entry - Persists a vnode's size and start cluster.
 *
 * The entry is located by name: a truncate to zero clears the start cluster,
 * so a cluster-keyed search would match a different zero-cluster entry.
 */
struct update_entry_ctx {
    struct vfs_vnode *node;
};

/*
 * Matches on the short name only, same as the loop this replaces -- unlike
 * lookup/unlink/rename it never matched on the assembled long name either, and
 * this refactor changes shape, not behavior.
 */
static enum fat32_walk_action update_entry_cb(struct fat32_dir_entry *entry, const char *lfn_name,
                                              int has_lfn, uint32_t lba, int s, int i, void *ctx_,
                                              int *out_err)
{
    struct update_entry_ctx *ctx = ctx_;
    (void)lfn_name;
    (void)has_lfn;

    if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) {
        return FAT32_WALK_CONTINUE;
    }
    if (!name_match(ctx->node->name, entry)) {
        return FAT32_WALK_CONTINUE;
    }

    uint32_t target_cluster = (uint32_t)(uintptr_t)ctx->node->internal_info;
    entry->size = (uint32_t)ctx->node->file_size;
    entry->cluster_high = (uint16_t)(target_cluster >> 16);
    entry->cluster_low = (uint16_t)(target_cluster & 0xFFFF);

    if (current_fs.dev->write_blocks(current_fs.dev, entry - i, lba + s, 1) != 0) {
        *out_err = -PERS_ERR_IO_ERROR;
        return FAT32_WALK_ERROR;
    }
    return FAT32_WALK_STOP;
}

static int fat32_update_dir_entry(struct vfs_vnode *node)
{
    if (!node->parent) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    uint32_t cluster = (uint32_t)(uintptr_t)node->parent->internal_info;
    struct update_entry_ctx ctx = {.node = node};
    return fat32_dir_walk(cluster, false, update_entry_cb, &ctx);
}

static int fat32_read_page(struct vfs_vnode *node, size_t page_index, void *page_buffer)
{
    uint32_t start_offset = page_index * PAGE_SIZE;
    if (start_offset >= node->file_size) {
        return 0;
    }

    uint32_t cluster = (uint32_t)(uintptr_t)node->internal_info;
    uint32_t bytes_per_cluster = current_fs.sectors_per_cluster * 512;
    uint32_t clusters_to_skip = start_offset / bytes_per_cluster;

    for (uint32_t i = 0; i < clusters_to_skip; i++) {
        cluster = get_next_cluster(cluster);
        if (cluster >= 0x0FFFFFF8) {
            return 0;
        }
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

    if (cluster == 0) {
        cluster = allocate_cluster();
        if (cluster == 0) {
            return -PERS_ERR_NO_SPACE_LEFT;
        }
        node->internal_info = (void *)(uintptr_t)cluster;
    }

    while (cluster_index < clusters_to_skip) {
        uint32_t next = get_next_cluster(cluster);
        if (next >= 0x0FFFFFF8) {
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

        // Read-modify-write for partial sector updates
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

static int fat32_vfs_read(struct vfs_file *file, void *buffer, size_t size, vfs_off_t *pos)
{
    if (!file || !file->node || !buffer) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    uint32_t file_size = (uint32_t)file->node->file_size;
    uint32_t offset = (uint32_t)*pos;

    if (offset >= file_size) {
        return 0;
    }
    if (offset + size > file_size) {
        size = file_size - offset;
    }

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
            void *fresh = pmm_alloc_pages(1);
            if (!fresh) {
                return bytes_read > 0 ? (int)bytes_read : -PERS_ERR_OUT_OF_MEMORY;
            }

            fat32_read_page(file->node, page_index, fresh);

            if (pagecache_add_page(file->node, page_index, fresh) == PERS_SUCCESS) {
                page_data = fresh; // add_page pinned it
            } else {
                pmm_free_pages(fresh);
                page_data = pagecache_get_page(file->node, page_index);
            }
        }

        if (page_data) {
            for (size_t i = 0; i < to_copy; i++) {
                out_buf[bytes_read + i] = ((uint8_t *)page_data)[offset_in_page + i];
            }
            pagecache_put_page(file->node, page_index);
        } else {
            break;
        }

        bytes_read += to_copy;
    }

    *pos += (vfs_off_t)bytes_read;
    return (int)bytes_read;
}

// Assigns a start cluster to a file that has none, recording it on disk.
static int fat32_ensure_start_cluster(struct vfs_vnode *node)
{
    if ((uint32_t)(uintptr_t)node->internal_info != 0) {
        return PERS_SUCCESS;
    }

    uint32_t cluster = allocate_cluster();
    if (cluster == 0) {
        return -PERS_ERR_NO_SPACE_LEFT;
    }

    node->internal_info = (void *)(uintptr_t)cluster;
    fat32_update_dir_entry(node);
    return PERS_SUCCESS;
}

static int fat32_vfs_write(struct vfs_file *file, const void *buffer, size_t size, vfs_off_t *pos)
{
    if (!file || !file->node || !buffer) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    if (file->node->type == VFS_VNODE_TYPE_DIR) {
        return -PERS_ERR_IS_A_DIRECTORY;
    }

    int cluster_err = fat32_ensure_start_cluster(file->node);
    if (cluster_err != PERS_SUCCESS) {
        return cluster_err;
    }

    uint32_t offset = (uint32_t)*pos;
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
            void *fresh = pmm_alloc_pages(1);
            if (!fresh) {
                return bytes_written > 0 ? (int)bytes_written : -PERS_ERR_OUT_OF_MEMORY;
            }
            memset(fresh, 0, PAGE_SIZE);

            if (page_index * PAGE_SIZE < (size_t)file->node->file_size) {
                fat32_read_page(file->node, page_index, fresh);
            }

            if (pagecache_add_page(file->node, page_index, fresh) == PERS_SUCCESS) {
                page_data = fresh; // add_page pinned it
            } else {
                pmm_free_pages(fresh);
                page_data = pagecache_get_page(file->node, page_index);
                if (!page_data) {
                    return bytes_written > 0 ? (int)bytes_written : -PERS_ERR_OUT_OF_MEMORY;
                }
            }
        }

        for (size_t i = 0; i < to_copy; i++) {
            ((uint8_t *)page_data)[offset_in_page + i] = in_buf[bytes_written + i];
        }

        pagecache_mark_dirty(file->node, page_index);

        // Write through to disk immediately
        size_t page_end = offset_in_page + to_copy;
        size_t valid_in_page = page_end;
        if ((page_index + 1) * PAGE_SIZE <= (size_t)file->node->file_size) {
            valid_in_page = PAGE_SIZE;
        }

        int write_result = fat32_write_page(file->node, page_index, page_data, valid_in_page);
        if (write_result < 0) {
            pagecache_put_page(file->node, page_index);
            return bytes_written > 0 ? (int)bytes_written : write_result;
        }

        pagecache_clear_dirty(file->node, page_index);
        pagecache_put_page(file->node, page_index);

        bytes_written += to_copy;
    }

    *pos += (vfs_off_t)bytes_written;

    if (*pos > file->node->file_size) {
        file->node->file_size = *pos;
        fat32_update_dir_entry(file->node);
    }

    return (int)bytes_written;
}

static struct vfs_vnode_ops fat32_vnode_ops;

static void name_to_83(const char *filename, uint8_t out_name[8], uint8_t out_ext[3])
{
    for (int k = 0; k < 8; k++) {
        out_name[k] = ' ';
    }
    for (int k = 0; k < 3; k++) {
        out_ext[k] = ' ';
    }

    int i = 0, j = 0;
    while (filename[i] != '.' && filename[i] != '\0' && j < 8) {
        char c = filename[i++];
        if (c >= 'a' && c <= 'z') {
            c -= 32;
        }
        out_name[j++] = c;
    }
    while (filename[i] != '.' && filename[i] != '\0') {
        i++;
    }
    if (filename[i] == '.') {
        i++;
        j = 0;
        while (filename[i] != '\0' && j < 3) {
            char c = filename[i++];
            if (c >= 'a' && c <= 'z') {
                c -= 32;
            }
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

struct write_entry_ctx {
    struct fat32_dir_entry *new_entry;
};

// Claims the first free slot (deleted or end-of-directory) instead of
// matching a name -- the "odd one out" among the callbacks.
static enum fat32_walk_action write_entry_cb(struct fat32_dir_entry *entry, const char *lfn_name,
                                             int has_lfn, uint32_t lba, int s, int i, void *ctx_,
                                             int *out_err)
{
    struct write_entry_ctx *ctx = ctx_;
    (void)lfn_name;
    (void)has_lfn;

    if (entry->name[0] != 0x00 && entry->name[0] != 0xE5) {
        return FAT32_WALK_CONTINUE;
    }

    *entry = *ctx->new_entry;
    if (current_fs.dev->write_blocks(current_fs.dev, entry - i, lba + s, 1) != 0) {
        *out_err = -PERS_ERR_IO_ERROR;
        return FAT32_WALK_ERROR;
    }
    return FAT32_WALK_STOP;
}

static int fat32_write_entry_to_parent(uint32_t parent_cluster, struct fat32_dir_entry *new_entry)
{
    struct write_entry_ctx ctx = {.new_entry = new_entry};
    int ret = fat32_dir_walk(parent_cluster, /*grow_chain=*/true, write_entry_cb, &ctx);
    // write_entry_cb claims the end-of-directory marker itself, so running off
    // the chain (NOT_FOUND) only happens if growing it failed outright --
    // report that the same way the old loop did.
    return (ret == -PERS_ERR_NOT_FOUND) ? -PERS_ERR_OUT_OF_MEMORY : ret;
}

struct lookup_ctx {
    const char *filename;
    struct vfs_vnode *dir;
    struct vfs_vnode *result;
};

int fat32_dir_walk(uint32_t parent_cluster, bool grow_chain, fat32_dir_walk_cb cb, void *ctx_)
{
    uint32_t cluster = parent_cluster;
    struct fat32_dir_entry dirs[16];

    char lfn_name[FAT32_LFN_BUF_SIZE];
    memset(lfn_name, 0, sizeof(lfn_name));
    int has_lfn = 0;

    while (cluster_valid(cluster)) {
        uint32_t lba = cluster_to_lba(cluster);
        for (int s = 0; s < (int)current_fs.sectors_per_cluster; s++) {
            if (current_fs.dev->read_blocks(current_fs.dev, &dirs, lba + s, 1) != 0) {
                return -PERS_ERR_IO_ERROR;
            }
            for (int i = 0; i < 16; i++) {
                // LFN-continuation entries are consumed here and never shown
                // to cb; a deleted or end-marker byte always wins over that,
                // matching the order the loop this replaces checked in.
                if (dirs[i].attributes == 0x0F && dirs[i].name[0] != 0x00
                    && dirs[i].name[0] != 0xE5) {
                    struct fat32_lfn_entry *lfn = (struct fat32_lfn_entry *)&dirs[i];
                    if (extract_lfn_part(lfn, lfn_name, sizeof(lfn_name)) != 0) {
                        /* Drop the partial name so a malformed fragment cannot
                         * leak into the match for a later entry. */
                        has_lfn = 0;
                        memset(lfn_name, 0, sizeof(lfn_name));
                        continue;
                    }
                    has_lfn = 1;
                    continue;
                }

                int out_err;
                enum fat32_walk_action action =
                    cb(&dirs[i], lfn_name, has_lfn, lba, s, i, ctx_, &out_err);

                switch (action) {
                    case FAT32_WALK_STOP:
                        return PERS_SUCCESS;
                    case FAT32_WALK_ERROR:
                        return out_err;
                    case FAT32_WALK_CONTINUE:
                        break;
                }

                has_lfn = 0;
                memset(lfn_name, 0, sizeof(lfn_name));

                // Nothing valid follows the end marker: stop the whole walk
                // right here, unless cb already claimed this slot above.
                if (dirs[i].name[0] == 0x00) {
                    return -PERS_ERR_NOT_FOUND;
                }
            }
        }

        uint32_t next = get_next_cluster(cluster);
        if (next >= 0x0FFFFFF8) {
            if (!grow_chain) {
                return -PERS_ERR_NOT_FOUND;
            }
            next = extend_cluster_chain(cluster);
            if (next == 0) {
                return -PERS_ERR_OUT_OF_MEMORY;
            }
        }
        cluster = next;
    }
    return -PERS_ERR_NOT_FOUND;
}

static enum fat32_walk_action lookup_cb(struct fat32_dir_entry *entry, const char *lfn_name,
                                        int has_lfn, uint32_t lba, int s, int i, void *ctx_,
                                        int *out_err)
{
    struct lookup_ctx *ctx = ctx_;
    (void)lba;
    (void)s;
    (void)i;

    if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) {
        return FAT32_WALK_CONTINUE;
    }
    if (!((has_lfn && strcmp(ctx->filename, lfn_name) == 0) || name_match(ctx->filename, entry))) {
        return FAT32_WALK_CONTINUE;
    }

    struct vfs_vnode *node = (struct vfs_vnode *)slab_alloc(sizeof(struct vfs_vnode));
    if (!node) {
        *out_err = -PERS_ERR_OUT_OF_MEMORY;
        return FAT32_WALK_ERROR;
    }
    memset(node, 0, sizeof(struct vfs_vnode));
    node->type = (entry->attributes & 0x10) ? VFS_VNODE_TYPE_DIR : VFS_VNODE_TYPE_REGULAR;
    node->ops = &fat32_vnode_ops;
    node->internal_info = (void *)(uintptr_t)((entry->cluster_high << 16) | entry->cluster_low);
    node->file_size = entry->size;
    node->parent = (struct vfs_vnode *)ctx->dir;
    atomic_inc(&ctx->dir->refcount);
    atomic_set(&node->refcount, 1);
    ctx->result = node;
    return FAT32_WALK_STOP;
}

static struct vfs_vnode *fat32_vfs_lookup(struct vfs_vnode *dir, const char *filename)
{
    struct lookup_ctx ctx = {.filename = filename, .dir = dir, .result = NULL};
    uint32_t cluster = (uint32_t)(uintptr_t)dir->internal_info;
    fat32_dir_walk(cluster, false, lookup_cb, &ctx);
    return ctx.result;
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

    char lfn_name[FAT32_LFN_BUF_SIZE];
    memset(lfn_name, 0, sizeof(lfn_name));
    int has_lfn = 0;

    struct fat32_dir_entry dirs[16];
    while (cluster_valid(cluster) && entries_read < (int)max_entries) {
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

                if (dirs[i].name[0] == 0x00) {
                    return entries_read;
                }

                if (dirs[i].name[0] == 0xE5) {
                    has_lfn = 0;
                    continue;
                }
                if (dirs[i].attributes == 0x0F) {
                    struct fat32_lfn_entry *lfn = (struct fat32_lfn_entry *)&dirs[i];
                    if (extract_lfn_part(lfn, lfn_name, sizeof(lfn_name)) != 0) {
                        /* Drop the partial name so a malformed fragment cannot
                         * leak into the match for a later entry. */
                        has_lfn = 0;
                        memset(lfn_name, 0, sizeof(lfn_name));
                        continue;
                    }
                    has_lfn = 1;
                    continue;
                }
                // Skip . and .. entries from the FAT filesystem itself
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
                memset(lfn_name, 0, sizeof(lfn_name));
                dirent->ino = (uint32_t)((dirs[i].cluster_high << 16) | dirs[i].cluster_low);
                entries_read++;
            }
        }
        cluster = get_next_cluster(cluster);
    }

    return entries_read;
}
struct unlink_ctx {
    const char *name;
};
static enum fat32_walk_action unlink_cb(struct fat32_dir_entry *entry, const char *lfn_name,
                                        int has_lfn, uint32_t lba, int s, int i, void *ctx_,
                                        int *out_err)
{
    struct unlink_ctx *ctx = ctx_;

    if ((has_lfn && strcmp(ctx->name, lfn_name) == 0) || name_match(ctx->name, entry)) {
        if (entry->attributes & 0x10) {
            *out_err = -PERS_ERR_IS_A_DIRECTORY;
            return FAT32_WALK_ERROR;
        }
        uint32_t target_cluster = (entry->cluster_high << 16) | entry->cluster_low;
        entry->name[0] = 0xE5;
        if (current_fs.dev->write_blocks(current_fs.dev, entry - i, lba + s, 1) != 0) {
            *out_err = -PERS_ERR_IO_ERROR;
            return FAT32_WALK_ERROR;
        }
        if (target_cluster >= 2) {
            fat32_free_cluster_chain(target_cluster);
        }
        return FAT32_WALK_STOP;
    }
    return FAT32_WALK_CONTINUE;
}

static int fat32_unlink(struct vfs_vnode *parent, const char *name)
{
    uint32_t cluster = (uint32_t)(uintptr_t)parent->internal_info;
    struct unlink_ctx ctx = {.name = name};
    return fat32_dir_walk(cluster, false, unlink_cb, &ctx);
}

// Empty-directory check used by rmdir_cb, as its own small walk over the
// target directory's own contents. Returns 1 if empty, 0 if not, or a
// negative error code.
static enum fat32_walk_action dir_empty_cb(struct fat32_dir_entry *entry, const char *lfn_name,
                                           int has_lfn, uint32_t lba, int s, int i, void *ctx_,
                                           int *out_err)
{
    (void)lfn_name;
    (void)has_lfn;
    (void)lba;
    (void)s;
    (void)i;
    (void)ctx_;
    (void)out_err;

    if (entry->name[0] == 0x00 || entry->name[0] == 0xE5 || entry->name[0] == '.') {
        return FAT32_WALK_CONTINUE;
    }
    return FAT32_WALK_STOP; // found a real entry: not empty
}

static int fat32_dir_is_empty(uint32_t cluster)
{
    int ret = fat32_dir_walk(cluster, false, dir_empty_cb, NULL);
    if (ret == -PERS_ERR_NOT_FOUND) {
        return 1; // walked off the end without seeing a real entry
    }
    if (ret == PERS_SUCCESS) {
        return 0; // dir_empty_cb stopped on a real entry
    }
    return ret;
}

struct rmdir_ctx {
    const char *name;
};

static enum fat32_walk_action rmdir_cb(struct fat32_dir_entry *entry, const char *lfn_name,
                                       int has_lfn, uint32_t lba, int s, int i, void *ctx_,
                                       int *out_err)
{
    struct rmdir_ctx *ctx = ctx_;

    if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) {
        return FAT32_WALK_CONTINUE;
    }
    if (!((has_lfn && strcmp(ctx->name, lfn_name) == 0) || name_match(ctx->name, entry))) {
        return FAT32_WALK_CONTINUE;
    }

    if (!(entry->attributes & 0x10)) {
        *out_err = -PERS_ERR_NOT_A_DIRECTORY;
        return FAT32_WALK_ERROR;
    }

    uint32_t target_cluster = (entry->cluster_high << 16) | entry->cluster_low;
    int empty = fat32_dir_is_empty(target_cluster);
    if (empty < 0) {
        *out_err = empty;
        return FAT32_WALK_ERROR;
    }
    if (!empty) {
        *out_err = -PERS_ERR_DIR_NOT_EMPTY;
        return FAT32_WALK_ERROR;
    }

    entry->name[0] = 0xE5;
    if (current_fs.dev->write_blocks(current_fs.dev, entry - i, lba + s, 1) != 0) {
        *out_err = -PERS_ERR_IO_ERROR;
        return FAT32_WALK_ERROR;
    }
    if (target_cluster >= 2) {
        fat32_free_cluster_chain(target_cluster);
    }
    return FAT32_WALK_STOP;
}

static int fat32_rmdir(struct vfs_vnode *parent, const char *name)
{
    uint32_t cluster = (uint32_t)(uintptr_t)parent->internal_info;
    struct rmdir_ctx ctx = {.name = name};
    return fat32_dir_walk(cluster, false, rmdir_cb, &ctx);
}

static int fat32_mkdir(struct vfs_vnode *parent, const char *name)
{
    if (strlen(name) > 255) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    struct vfs_vnode *existing = fat32_vfs_lookup(parent, name);
    if (existing) {
        vfs_vnode_put(existing);
        return -PERS_ERR_ALREADY_EXISTS;
    }

    uint32_t new_cluster = allocate_cluster();
    if (new_cluster == 0) {
        return -PERS_ERR_OUT_OF_MEMORY;
    }

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

struct rename_find_ctx {
    const char *name;
    struct fat32_dir_entry entry;
    uint32_t lba;
    int s;
    int i;
};

static enum fat32_walk_action rename_find_cb(struct fat32_dir_entry *entry, const char *lfn_name,
                                             int has_lfn, uint32_t lba, int s, int i, void *ctx_,
                                             int *out_err)
{
    struct rename_find_ctx *ctx = ctx_;
    (void)out_err;

    if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) {
        return FAT32_WALK_CONTINUE;
    }
    if (!((has_lfn && strcmp(ctx->name, lfn_name) == 0) || name_match(ctx->name, entry))) {
        return FAT32_WALK_CONTINUE;
    }

    ctx->entry = *entry;
    ctx->lba = lba;
    ctx->s = s;
    ctx->i = i;
    return FAT32_WALK_STOP;
}

static int fat32_rename(struct vfs_vnode *old_parent, const char *old_name,
                        struct vfs_vnode *new_parent, const char *new_name)
{
    uint32_t old_cluster = (uint32_t)(uintptr_t)old_parent->internal_info;
    struct rename_find_ctx ctx = {.name = old_name};
    int found = fat32_dir_walk(old_cluster, false, rename_find_cb, &ctx);
    if (found != PERS_SUCCESS) {
        return found;
    }

    struct fat32_dir_entry new_entry = ctx.entry;
    name_to_83(new_name, new_entry.name, new_entry.ext);

    uint32_t new_parent_cluster = (uint32_t)(uintptr_t)new_parent->internal_info;
    int res = fat32_write_entry_to_parent(new_parent_cluster, &new_entry);
    if (res != PERS_SUCCESS) {
        return res;
    }

    // The insert above may have grown/rewritten clusters, so re-read the old
    // slot fresh rather than trusting a buffer from the earlier walk.
    struct fat32_dir_entry dirs[16];
    if (current_fs.dev->read_blocks(current_fs.dev, &dirs, ctx.lba + ctx.s, 1) != 0) {
        return -PERS_ERR_IO_ERROR;
    }
    dirs[ctx.i].name[0] = 0xE5;
    if (current_fs.dev->write_blocks(current_fs.dev, &dirs, ctx.lba + ctx.s, 1) != 0) {
        return -PERS_ERR_IO_ERROR;
    }

    return PERS_SUCCESS;
}

static int fat32_create(struct vfs_vnode *parent, const char *name)
{
    /*
     * Allocate the first cluster up front so the file has a unique, stable
     * start cluster from creation. internal_info (the start cluster) is the
     * page-cache key; leaving it 0 until the first write makes every empty
     * file alias the same key and cross-contaminate their cached pages.
     */
    uint32_t new_cluster = allocate_cluster();
    if (new_cluster == 0) {
        return -PERS_ERR_NO_SPACE_LEFT;
    }

    // Zero the cluster so stale data from a deleted file cannot surface.
    uint8_t zero_sector[512];
    memset(zero_sector, 0, sizeof(zero_sector));
    for (uint32_t s = 0; s < current_fs.sectors_per_cluster; s++) {
        if (current_fs.dev->write_blocks(current_fs.dev, zero_sector,
                                         cluster_to_lba(new_cluster) + s, 1)
            != 0) {
            fat32_free_cluster_chain(new_cluster);
            return -PERS_ERR_IO_ERROR;
        }
    }

    struct fat32_dir_entry new_entry;
    memset(&new_entry, 0, sizeof(new_entry));
    name_to_83(name, new_entry.name, new_entry.ext);
    new_entry.attributes = 0x20; // archive = regular file
    new_entry.cluster_high = (uint16_t)(new_cluster >> 16);
    new_entry.cluster_low = (uint16_t)(new_cluster & 0xFFFF);
    new_entry.size = 0;

    uint32_t parent_cluster = (uint32_t)(uintptr_t)parent->internal_info;
    int res = fat32_write_entry_to_parent(parent_cluster, &new_entry);
    if (res != PERS_SUCCESS) {
        fat32_free_cluster_chain(new_cluster);
    }
    return res;
}

/*
 * Registered vnode ops. Each is a thin wrapper that serializes a FAT32 entry
 * point under fat32_lock; the internal helpers above assume the lock is held.
 * The lock is recursive so page-cache eviction can re-enter write_page while a
 * read/write already holds it.
 */
static int fat32_op_read(struct vfs_file *file, void *buffer, size_t size, vfs_off_t *pos)
{
    kmutex_lock(&fat32_lock);
    int r = fat32_vfs_read(file, buffer, size, pos);
    kmutex_unlock(&fat32_lock);
    return r;
}

static int fat32_op_write(struct vfs_file *file, const void *buffer, size_t size, vfs_off_t *pos)
{
    kmutex_lock(&fat32_lock);
    int r = fat32_vfs_write(file, buffer, size, pos);
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

static int fat32_truncate(struct vfs_vnode *node, vfs_off_t length)
{
    if (length > node->file_size) {
        return -PERS_ERR_OPERATION_NOT_SUPPORTED; // grow: TODO, first cut
    }
    if (length == node->file_size) {
        return PERS_SUCCESS; // no-op
    }

    pagecache_invalidate(node);

    uint32_t start_cluster = (uint32_t)(uintptr_t)node->internal_info;

    if (length == 0) {
        if (cluster_valid(start_cluster)) {
            fat32_free_cluster_chain(start_cluster);
        }
        node->internal_info = (void *)(uintptr_t)0;
    } else {
        uint32_t bytes_per_cluster = current_fs.sectors_per_cluster * 512;
        uint32_t cluster_index = (uint32_t)((length - 1) / bytes_per_cluster);
        uint32_t last_keep = start_cluster;

        for (uint32_t i = 0; i < cluster_index; i++) {
            if (!cluster_valid(last_keep)) {
                break;
            }
            last_keep = get_next_cluster(last_keep);
        }

        if (cluster_valid(last_keep)) {
            uint32_t next = get_next_cluster(last_keep);
            set_fat_entry(last_keep, 0x0FFFFFFF);
            if (cluster_valid(next)) {
                fat32_free_cluster_chain(next);
            }
        }
    }

    node->file_size = length;
    fat32_update_dir_entry(node);
    return PERS_SUCCESS;
}

static int fat32_op_truncate(struct vfs_vnode *node, vfs_off_t length)
{
    kmutex_lock(&fat32_lock);
    int r = fat32_truncate(node, length);
    kmutex_unlock(&fat32_lock);
    return r;
}

static struct vfs_vnode_ops fat32_vnode_ops = {
    .read = fat32_op_read,
    .write = fat32_op_write,
    .truncate = fat32_op_truncate,
    .lookup = fat32_op_lookup,
    .readdir = fat32_op_readdir,
    .write_page = fat32_op_write_page,
    .mkdir = fat32_op_mkdir,
    .create = fat32_op_create,
    .rmdir = fat32_op_rmdir,
    .unlink = fat32_op_unlink,
    .rename = fat32_op_rename,
};

struct vfs_vnode *fat32_get_root_node(void)
{
    struct vfs_vnode *node = slab_alloc(sizeof(struct vfs_vnode));
    if (!node) {
        return NULL;
    }
    memset(node, 0, sizeof(*node));
    node->type = VFS_VNODE_TYPE_DIR;
    node->ops = &fat32_vnode_ops;
    node->internal_info = (void *)(uintptr_t)current_fs.root_cluster;
    atomic_set(&node->refcount, 1);
    return node;
}

/*
 * fat32_geometry_from_bpb - Validates a BPB and derives the runtime geometry.
 *
 * Every field here is attacker-controlled on a removable volume, and the driver
 * divides by some of them and turns others into block addresses. Reject a
 * volume that cannot be described rather than mount it and misbehave later.
 */
static int fat32_geometry_from_bpb(const struct fat32_bpb *bpb, uint32_t partition_lba,
                                   uint64_t device_blocks, struct fat32_fs *out)
{
    uint32_t spc = bpb->sectors_per_cluster;
    uint32_t reserved = bpb->reserved_sectors;
    uint32_t num_fats = bpb->num_fats;
    uint32_t sectors_per_fat = bpb->sectors_per_fat_32;

    if (bpb->bytes_per_sector != FAT32_SECTOR_SIZE) {
        return -PERS_ERR_OPERATION_NOT_SUPPORTED;
    }

    /* A power of two up to 128 keeps a cluster within the 64 KB the spec allows
     * and, more importantly here, keeps it non-zero: it is a divisor. */
    if (spc == 0 || spc > 128 || (spc & (spc - 1)) != 0) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }
    if (reserved == 0 || sectors_per_fat == 0 || num_fats < 1 || num_fats > 2) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }
    if (bpb->root_cluster < 2) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    /* Compute in 64 bits: num_fats * sectors_per_fat overflows a uint32_t for
     * plausible-looking values, wrapping data_lba_start back over the FAT. */
    uint64_t fat_lba = (uint64_t)partition_lba + reserved;
    uint64_t data_lba = fat_lba + (uint64_t)num_fats * sectors_per_fat;

    if (data_lba >= device_blocks) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    uint64_t cluster_count = (device_blocks - data_lba) / spc;
    if (cluster_count == 0) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    // Clusters are numbered from 2, and the top values are reserved markers.
    uint64_t max_cluster = cluster_count + 1;
    if (max_cluster > FAT32_CLUSTER_MAX) {
        max_cluster = FAT32_CLUSTER_MAX;
    }
    if (bpb->root_cluster > max_cluster) {
        return -PERS_ERR_INVALID_ARGUMENT;
    }

    out->bytes_per_sector = FAT32_SECTOR_SIZE;
    out->sectors_per_cluster = spc;
    out->reserved_sectors = reserved;
    out->num_fats = num_fats;
    out->sectors_per_fat = sectors_per_fat;
    out->root_cluster = bpb->root_cluster;
    out->partition_lba_start = partition_lba;
    out->fat_lba_start = (uint32_t)fat_lba;
    out->data_lba_start = (uint32_t)data_lba;
    out->max_cluster = (uint32_t)max_cluster;

    return PERS_SUCCESS;
}

#ifdef CONFIG_TESTS
int fat32_test_geometry_from_bpb(const struct fat32_bpb *bpb, uint32_t partition_lba,
                                 uint64_t device_blocks, struct fat32_fs *out)
{
    return fat32_geometry_from_bpb(bpb, partition_lba, device_blocks, out);
}
#endif

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

    int geom = fat32_geometry_from_bpb(&bpb, current_fs.partition_lba_start,
                                       (uint64_t)dev->block_count, &current_fs);
    if (geom != PERS_SUCCESS) {
        pr_err("fat32: rejecting volume with an implausible BPB\n");
        current_fs.dev = NULL;
        return geom;
    }

    pr_info("fat32: partition at LBA %u, %u clusters of %u sectors\n",
            current_fs.partition_lba_start, current_fs.max_cluster - 1,
            current_fs.sectors_per_cluster);
    return PERS_SUCCESS;
}
