/*
 * fat32.c - Implementation of the FAT32 filesystem driver.
 */

#include "fs/fat32.h"
#include "core/timer.h"

#include "fs/vfs.h"
#include "stdio.h"
#include "string.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "uapi/errno.h"

#include "core/lock.h"
#include "core/mutex.h"
#include "mm/slab.h"
#include "fs/pagecache.h"
#include "mm/pmm.h"
#include "mm/addr.h"

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
 * sleeping mutex (a spinlock can't be held across sched_schedule()). It is recursive
 * because a read/write already holding it can re-enter write_page via the page
 * cache eviction path.
 */
static struct kmutex fat32_lock = KMUTEX_INIT;

// Every read and write path in this driver assumes 512-byte sectors.
#define FAT32_SECTOR_SIZE 512

// Highest cluster number FAT32 can address; above this are reserved markers.
#define FAT32_CLUSTER_MAX 0x0FFFFFF6U

// FAT cluster values at or above this are the reserved range 0x0FFFFFF8 to
// 0x0FFFFFFF: end-of-chain and bad-cluster markers. Nothing in this driver
// distinguishes the two, so one threshold covers both.
#define FAT32_CLUSTER_EOC_MIN 0x0FFFFFF8U
// Canonical "chain terminates here" value written into a FAT entry.
#define FAT32_CLUSTER_EOC 0x0FFFFFFFU
// A raw 32-bit FAT entry's top 4 bits are reserved and must be masked off
// before the value is used as a cluster number.
#define FAT32_CLUSTER_MASK 0x0FFFFFFFU

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
 * fat32_vfs_readdir does NOT go through this walker: it has to resume a
 * listing from an arbitrary byte offset across separate syscalls, which is
 * state this walker's "run to completion or to a cb-decided stop" contract
 * has no room for. It keeps its own copy of the same cluster/sector/entry
 * scan deliberately, not as an oversight.
 *
 * Declared up here because fat32_update_dir_entry (below) needs it before its
 * definition further down the file, next to its first caller.
 */
enum fat32_walk_action {
    FAT32_WALK_CONTINUE = 0, // not it -- keep scanning
    FAT32_WALK_STOP = 1,     // found what I wanted, stop the whole walk
    FAT32_WALK_ERROR = 2,    // cb hit its own failure; error is left in *out_err
};

/*
 * fat32_dir_slot - Where one 32-byte directory entry lives on disk.
 */
struct fat32_dir_slot {
    uint32_t lba; // absolute sector LBA
    int index;    // entry within that sector, 0..15
};

// A long name is at most 255 characters, 13 per LFN entry.
#define FAT32_LFN_MAX_ENTRIES 20

// Ceiling on a directory's cluster chain; a chain that loops has no other one.
#define FAT32_MAX_DIR_CLUSTERS 65536U

/*
 * fat32_dir_pos - The entry's position, and the LFN entries that name it.
 *
 * lfn_slots is what lets a caller remove a name completely: the LFN entries
 * sit immediately before the 8.3 entry and are invisible to cb otherwise, so
 * deleting only the entry cb was handed would leave them behind as orphans
 * that still carry the old name.
 */
struct fat32_dir_pos {
    uint32_t lba;
    int sector_index;
    int entry_index;

    const char *lfn_name;
    int has_lfn;

    const struct fat32_dir_slot *lfn_slots;
    int lfn_count;

    uint32_t ordinal; // slot number from the start of the directory, LFN slots included
};

/*
 * cb sets *dirty to persist any change it made to *entry: fat32_dir_walk
 * writes the whole sector back itself, so cb never needs to reconstruct the
 * sector buffer or call write_blocks on its own.
 */
typedef enum fat32_walk_action (*fat32_dir_walk_cb)(struct fat32_dir_entry *entry,
                                                    const struct fat32_dir_pos *pos, void *ctx,
                                                    int *out_err, int *dirty);

static int fat32_dir_walk(uint32_t parent_cluster, int grow_chain, fat32_dir_walk_cb cb, void *ctx);

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

// A FAT that could not be read. Past any masked entry, so a walker stops on
// it, but a caller about to extend the chain must not: the link is still live.
#define FAT32_CLUSTER_IOERR 0xFFFFFFFFU

static uint32_t get_next_cluster(uint32_t cluster)
{
    // Terminate the chain rather than index the FAT with a corrupt value.
    if (!cluster_valid(cluster)) {
        return FAT32_CLUSTER_EOC;
    }

    uint32_t fat_sector = current_fs.fat_lba_start + (cluster / 128);
    uint32_t fat_offset = cluster % 128;
    uint32_t fat_buffer[128];

    if (current_fs.dev->read_blocks(current_fs.dev, &fat_buffer, fat_sector, 1) != 0) {
        return FAT32_CLUSTER_IOERR;
    }

    return fat_buffer[fat_offset] & FAT32_CLUSTER_MASK;
}

/*
 * set_fat_entry - Writes a value to a FAT entry for a given cluster.
 *
 * Updates all FAT copies to maintain filesystem consistency.
 */
static int set_fat_entry(uint32_t cluster, uint32_t value)
{
    // An out-of-range cluster addresses a sector past the FAT, over file data.
    if (!cluster_valid(cluster)) {
        return -EINVAL;
    }

    uint32_t fat_sector_offset = cluster / 128;
    uint32_t fat_offset = cluster % 128;
    uint32_t fat_buffer[128];

    for (uint32_t i = 0; i < current_fs.num_fats; i++) {
        uint32_t fat_sector =
            current_fs.fat_lba_start + (i * current_fs.sectors_per_fat) + fat_sector_offset;

        if (current_fs.dev->read_blocks(current_fs.dev, fat_buffer, fat_sector, 1) != 0) {
            return -EIO;
        }

        fat_buffer[fat_offset] = value & FAT32_CLUSTER_MASK;

        if (current_fs.dev->write_blocks(current_fs.dev, fat_buffer, fat_sector, 1) != 0) {
            return -EIO;
        }
    }

    return 0;
}

/*
 * allocate_cluster - Finds a free cluster in the FAT and marks it as end-of-chain.
 *
 * Returns the cluster number on success, or 0 on failure.
 */
/*
 * Where the last search left off. Restarting from cluster 2 on every call
 * makes filling a disk quadratic: each allocation re-reads and re-scans every
 * FAT sector already known to be full. The hint is advisory -- a wrong value
 * costs a wasted scan, never a wrong answer -- so it needs no locking beyond
 * the fat32 mutex already held here, and nothing goes wrong if it is stale.
 *
 * FAT32 keeps the same hint on disk in the FSInfo sector; reading it at mount
 * would carry the benefit across boots. Writing it back is what makes that
 * safe, and that belongs with the rest of the FSInfo bookkeeping.
 */
static uint32_t next_free_hint = 2;

static uint32_t allocate_cluster(void)
{
    uint32_t fat_buffer[128];
    uint32_t last_sector = (uint32_t)-1;

    if (next_free_hint < 2 || next_free_hint > current_fs.max_cluster) {
        next_free_hint = 2;
    }

    // From the hint to the end, then wrap and cover what was skipped.
    uint32_t total = current_fs.max_cluster - 1;
    for (uint32_t n = 0; n < total; n++) {
        uint32_t cluster = next_free_hint + n;
        if (cluster > current_fs.max_cluster) {
            cluster = 2 + (cluster - current_fs.max_cluster - 1);
        }

        uint32_t fat_sector = current_fs.fat_lba_start + (cluster / 128);
        uint32_t fat_offset = cluster % 128;

        if (fat_sector != last_sector) {
            if (current_fs.dev->read_blocks(current_fs.dev, fat_buffer, fat_sector, 1) != 0) {
                return 0;
            }
            last_sector = fat_sector;
        }

        uint32_t entry = fat_buffer[fat_offset] & FAT32_CLUSTER_MASK;
        if (entry == 0) {
            if (set_fat_entry(cluster, FAT32_CLUSTER_EOC) != 0) {
                return 0;
            }
            next_free_hint = (cluster < current_fs.max_cluster) ? cluster + 1 : 2;
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
/*
 * zero_cluster - Blanks every sector of a cluster.
 *
 * A directory cluster must read back as free slots. allocate_cluster hands out
 * whatever the cluster last held, so a directory that grows would otherwise
 * find file data where it expects entries, and read it as one.
 */
static int zero_cluster(uint32_t cluster)
{
    uint8_t zero_sector[512];
    memset(zero_sector, 0, sizeof(zero_sector));

    uint32_t base = cluster_to_lba(cluster);
    for (uint32_t s = 0; s < current_fs.sectors_per_cluster; s++) {
        if (current_fs.dev->write_blocks(current_fs.dev, zero_sector, base + s, 1) != 0) {
            return -EIO;
        }
    }
    return 0;
}

// The spec writes the root as cluster 0 in a "..", whatever its real number.
static uint32_t dotdot_cluster(uint32_t parent_cluster)
{
    return parent_cluster == current_fs.root_cluster ? 0 : parent_cluster;
}

static uint32_t extend_cluster_chain(uint32_t last_cluster)
{
    uint32_t new_cluster = allocate_cluster();
    if (new_cluster == 0) {
        return 0;
    }

    if (last_cluster != 0) {
        if (set_fat_entry(last_cluster, new_cluster) != 0) {
            set_fat_entry(new_cluster, 0);
            return 0;
        }
    }

    return new_cluster;
}

static char fold_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static int name_eq_nocase(const char *a, const char *b)
{
    for (; *a && fold_upper(*a) == fold_upper(*b); a++, b++) {}
    return fold_upper(*a) == fold_upper(*b);
}

/*
 * name_as_83 - Spells a name the way an 8.3 entry holds it, case folded.
 *
 * False when the name has no exact 8.3 spelling. It is never cut down to fit:
 * a long name lives under LFN entries and an alias, and a truncated spelling
 * would match some other file's short name.
 */
static int name_as_83(const char *name, uint8_t out_name[8], uint8_t out_ext[3])
{
    memset(out_name, ' ', 8);
    memset(out_ext, ' ', 3);

    if (name[0] == '\0' || name[0] == '.') {
        return 0;
    }

    int i = 0;
    for (int j = 0; name[i] != '\0' && name[i] != '.'; i++) {
        if (j == 8) {
            return 0;
        }
        out_name[j++] = (uint8_t)fold_upper(name[i]);
    }
    if (name[i] == '.') {
        i++;
        for (int j = 0; name[i] != '\0'; i++) {
            if (name[i] == '.' || j == 3) {
                return 0;
            }
            out_ext[j++] = (uint8_t)fold_upper(name[i]);
        }
    }
    return 1;
}

/*
 * entry_named - Whether a name is how an entry is known: its long name, or an
 * exact spelling of its short one. Neither compares case, as on any FAT.
 */
static int entry_named(const char *name, const struct fat32_dir_entry *entry,
                       const struct fat32_dir_pos *pos)
{
    if (pos->has_lfn && name_eq_nocase(name, pos->lfn_name)) {
        return 1;
    }

    uint8_t short_name[8], short_ext[3];
    return name_as_83(name, short_name, short_ext) && memcmp(short_name, entry->name, 8) == 0
           && memcmp(short_ext, entry->ext, 3) == 0;
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

static void fat32_stamp_entry(struct fat32_dir_entry *entry, int created);

/*
 * Matches the long name as well as the 8.3 one. A file created with a name
 * that does not fit 8.3 is stored under a generated alias, so the short name
 * no longer resembles what the vnode is called -- matching only on it would
 * silently fail to find the entry, and the size would never reach the disk.
 */
static enum fat32_walk_action update_entry_cb(struct fat32_dir_entry *entry,
                                              const struct fat32_dir_pos *pos, void *ctx_,
                                              int *out_err, int *dirty)
{
    struct update_entry_ctx *ctx = ctx_;
    (void)out_err;

    if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) {
        return FAT32_WALK_CONTINUE;
    }
    if (!entry_named(ctx->node->name, entry, pos)) {
        return FAT32_WALK_CONTINUE;
    }

    uint32_t target_cluster = (uint32_t)(uintptr_t)ctx->node->internal_info;
    entry->size = (uint32_t)ctx->node->file_size;
    entry->cluster_high = (uint16_t)(target_cluster >> 16);
    entry->cluster_low = (uint16_t)(target_cluster & 0xFFFF);
    fat32_stamp_entry(entry, 0);
    *dirty = 1;
    return FAT32_WALK_STOP;
}

static int fat32_update_dir_entry(struct vfs_vnode *node)
{
    if (!node->parent) {
        return -EINVAL;
    }

    uint32_t cluster = (uint32_t)(uintptr_t)node->parent->internal_info;
    struct update_entry_ctx ctx = {.node = node};
    return fat32_dir_walk(cluster, 0, update_entry_cb, &ctx);
}

// update_entry_cb's search, reading the entry instead of writing it.
static enum fat32_walk_action revalidate_cb(struct fat32_dir_entry *entry,
                                            const struct fat32_dir_pos *pos, void *ctx_,
                                            int *out_err, int *dirty)
{
    struct update_entry_ctx *ctx = ctx_;
    (void)out_err;
    (void)dirty;

    if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) {
        return FAT32_WALK_CONTINUE;
    }
    if (!entry_named(ctx->node->name, entry, pos)) {
        return FAT32_WALK_CONTINUE;
    }

    ctx->node->file_size = entry->size;
    ctx->node->internal_info =
        (void *)(uintptr_t)(((uint32_t)entry->cluster_high << 16) | entry->cluster_low);
    return FAT32_WALK_STOP;
}

/*
 * Bumped by every operation that can change a directory entry. A vnode stamped
 * with the current value holds what its entry says, so revalidating it can
 * skip the walk.
 */
static unsigned long fat32_entry_gen = 1;

static int fat32_revalidate(struct vfs_vnode *node)
{
    if (!node || node->type != VFS_VNODE_TYPE_REGULAR || !node->parent) {
        return 0;
    }
    if (node->revalidated == fat32_entry_gen) {
        return 0;
    }

    uint32_t cluster = (uint32_t)(uintptr_t)node->parent->internal_info;
    struct update_entry_ctx ctx = {.node = node};
    int r = fat32_dir_walk(cluster, 0, revalidate_cb, &ctx);

    // No entry is not an error: an unlinked file stays open through its fd.
    if (r != 0 && r != -ENOENT) {
        return r;
    }
    node->revalidated = fat32_entry_gen;
    return 0;
}

static int fat32_read_page(struct vfs_vnode *node, size_t page_index, void *page_buffer)
{
    memset(page_buffer, 0, PAGE_SIZE);

    uint32_t start_offset = page_index * PAGE_SIZE;
    if (start_offset >= node->file_size) {
        return 0;
    }

    uint32_t cluster = (uint32_t)(uintptr_t)node->internal_info;
    uint32_t bytes_per_cluster = current_fs.sectors_per_cluster * 512;
    uint32_t clusters_to_skip = start_offset / bytes_per_cluster;

    // Below the recorded size every byte has to come off the disk: a chain that
    // ends early, or a sector that will not read, is an error and not zeros.
    for (uint32_t i = 0; i < clusters_to_skip; i++) {
        cluster = get_next_cluster(cluster);
        if (cluster >= FAT32_CLUSTER_EOC_MIN) {
            return -EIO;
        }
    }

    uint32_t bytes_read = 0;
    uint32_t to_read = PAGE_SIZE;
    if (start_offset + to_read > node->file_size) {
        to_read = node->file_size - start_offset;
    }

    uint8_t sector_buffer[512];
    uint32_t current_offset = start_offset;

    while (bytes_read < to_read) {
        uint32_t offset_in_cluster = current_offset % bytes_per_cluster;
        uint32_t sector_in_cluster = offset_in_cluster / 512;
        uint32_t offset_in_sector = offset_in_cluster % 512;

        uint32_t lba = cluster_to_lba(cluster) + sector_in_cluster;

        if (current_fs.dev->read_blocks(current_fs.dev, sector_buffer, lba, 1) != 0) {
            return -EIO;
        }

        uint32_t can_read = 512 - offset_in_sector;
        uint32_t remaining = to_read - bytes_read;
        uint32_t to_copy = (can_read < remaining) ? can_read : remaining;

        memcpy((uint8_t *)page_buffer + bytes_read, sector_buffer + offset_in_sector, to_copy);

        bytes_read += to_copy;
        current_offset += to_copy;

        if (current_offset % bytes_per_cluster == 0) {
            cluster = get_next_cluster(cluster);
            if (cluster >= FAT32_CLUSTER_EOC_MIN && bytes_read < to_read) {
                return -EIO;
            }
        }
    }
    return 0;
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
            return -ENOSPC;
        }
        node->internal_info = (void *)(uintptr_t)cluster;
        fat32_update_dir_entry(node);
    }

    while (cluster_index < clusters_to_skip) {
        uint32_t next = get_next_cluster(cluster);
        if (next == FAT32_CLUSTER_IOERR) {
            return -EIO;
        }
        if (next >= FAT32_CLUSTER_EOC_MIN) {
            next = extend_cluster_chain(cluster);
            if (next == 0) {
                return -ENOSPC;
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

        memcpy(sector_buffer + offset_in_sector, (uint8_t *)page_buffer + bytes_written, to_copy);

        if (current_fs.dev->write_blocks(current_fs.dev, sector_buffer, lba, 1) != 0) {
            return bytes_written > 0 ? (int)bytes_written : -EIO;
        }

        bytes_written += to_copy;
        current_offset += to_copy;

        if (current_offset % bytes_per_cluster == 0 && bytes_written < valid_bytes) {
            uint32_t next = get_next_cluster(cluster);
            if (next == FAT32_CLUSTER_IOERR) {
                return bytes_written > 0 ? (int)bytes_written : -EIO;
            }
            if (next >= FAT32_CLUSTER_EOC_MIN) {
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
        return -EINVAL;
    }

    // Truncating to 32 bits would read from the wrong place instead of EOF.
    if (*pos < 0 || (uint64_t)*pos > UINT32_MAX) {
        return 0;
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
    int err = 0;

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
            void *fresh = pmm_alloc_pages_nozero(1);
            if (!fresh) {
                err = -ENOMEM;
                break;
            }

            int rerr = fat32_read_page(file->node, page_index, fresh);
            if (rerr != 0) {
                pmm_free_pages(fresh);
                err = rerr;
                break;
            }

            if (pagecache_add_page(file->node, page_index, fresh) == 0) {
                page_data = fresh; // add_page pinned it
            } else {
                pmm_free_pages(fresh);
                page_data = pagecache_get_page(file->node, page_index);
            }
        }

        if (page_data) {
            memcpy(out_buf + bytes_read, (uint8_t *)page_data + offset_in_page, to_copy);
            pagecache_put_page(file->node, page_index);
        } else {
            err = -ENOMEM;
            break;
        }

        bytes_read += to_copy;
    }

    if (bytes_read == 0 && err != 0) {
        return err;
    }

    *pos += (vfs_off_t)bytes_read;
    return (int)bytes_read;
}

// Assigns a start cluster to a file that has none, recording it on disk.
static int fat32_ensure_start_cluster(struct vfs_vnode *node)
{
    if ((uint32_t)(uintptr_t)node->internal_info != 0) {
        return 0;
    }

    uint32_t cluster = allocate_cluster();
    if (cluster == 0) {
        return -ENOSPC;
    }

    node->internal_info = (void *)(uintptr_t)cluster;
    fat32_update_dir_entry(node);
    return 0;
}

static int fat32_vfs_write(struct vfs_file *file, const void *buffer, size_t size, vfs_off_t *pos)
{
    if (!file || !file->node || !buffer) {
        return -EINVAL;
    }

    if (file->node->type == VFS_VNODE_TYPE_DIR) {
        return -EISDIR;
    }

    // A FAT32 size field is 32 bits; past that is unwritable, not wrappable.
    if (*pos < 0 || (uint64_t)*pos + size > UINT32_MAX) {
        return -EFBIG;
    }
    if (size == 0) {
        return 0;
    }

    int cluster_err = fat32_ensure_start_cluster(file->node);
    if (cluster_err != 0) {
        return cluster_err;
    }

    uint32_t offset = (uint32_t)*pos;
    uint32_t bytes_written = 0;
    const uint8_t *in_buf = (const uint8_t *)buffer;
    int err = 0;

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
            void *fresh = pmm_alloc_pages_nozero(1);
            if (!fresh) {
                err = -ENOMEM;
                break;
            }

            // The bytes around the write go back to disk with it.
            int rerr = fat32_read_page(file->node, page_index, fresh);
            if (rerr != 0) {
                pmm_free_pages(fresh);
                err = rerr;
                break;
            }

            if (pagecache_add_page(file->node, page_index, fresh) == 0) {
                page_data = fresh; // add_page pinned it
            } else {
                pmm_free_pages(fresh);
                page_data = pagecache_get_page(file->node, page_index);
                if (!page_data) {
                    err = -ENOMEM;
                    break;
                }
            }
        }

        memcpy((uint8_t *)page_data + offset_in_page, in_buf + bytes_written, to_copy);

        pagecache_mark_dirty(file->node, page_index);

        // Write through to disk immediately
        size_t page_end = offset_in_page + to_copy;
        size_t valid_in_page = page_end;
        if ((page_index + 1) * PAGE_SIZE <= (size_t)file->node->file_size) {
            valid_in_page = PAGE_SIZE;
        }

        /* A short count means the volume filled or a sector failed part-way
         * through the page. The page stays dirty so a later sync can retry it,
         * and the caller must not be told those bytes reached the disk. */
        int write_result = fat32_write_page(file->node, page_index, page_data, valid_in_page);
        if (write_result < 0 || (size_t)write_result < valid_in_page) {
            pagecache_put_page(file->node, page_index);
            err = write_result < 0 ? write_result : -ENOSPC;
            break;
        }

        pagecache_clear_dirty(file->node, page_index);
        pagecache_put_page(file->node, page_index);

        bytes_written += to_copy;
    }

    if (bytes_written == 0) {
        return err;
    }

    // What landed before a failure is the caller's: the offset and the size
    // have to cover it, or a retry overwrites it.
    *pos += (vfs_off_t)bytes_written;
    if (*pos > file->node->file_size) {
        file->node->file_size = *pos;
    }

    // Also the modification time, which moves whether or not the file grew.
    fat32_update_dir_entry(file->node);
    return (int)bytes_written;
}

static struct vfs_vnode_ops fat32_vnode_ops;

/*
 * fat32_stamp_entry - Fills in an entry's dates from the wall clock.
 *
 * FAT packs a date as (year-1980)<<9 | month<<5 | day and a time as
 * hour<<11 | minute<<5 | second/2 -- two-second resolution is the format's,
 * not ours. Everything is local time with no zone recorded, which is why two
 * systems can disagree about when the same file was written.
 */
static void fat32_stamp_entry(struct fat32_dir_entry *entry, int created)
{
    int64_t secs = (int64_t)timer_get_wall_time();
    int64_t days = secs / 86400;
    int64_t rem = secs % 86400;

    // Inverse of the era arithmetic in timer_get_wall_time.
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);

    if (y < 1980) {
        y = 1980; // the format cannot represent anything earlier
    }

    uint16_t date = (uint16_t)(((y - 1980) << 9) | (m << 5) | d);
    uint16_t time = (uint16_t)(((rem / 3600) << 11) | ((rem % 3600 / 60) << 5) | (rem % 60 / 2));

    entry->last_mod_date = date;
    entry->last_mod_time = time;
    entry->last_access_date = date;

    if (created) {
        entry->create_date = date;
        entry->create_time = time;
        entry->create_time_ms = 0;
    }
}

// Longest name a directory entry can carry, in characters.
#define FAT32_LFN_MAX_NAME 255

/*
 * fat32_put_slot - Writes one 32-byte entry into its slot on disk.
 *
 * Read-modify-write of the containing sector: a run of LFN entries can span
 * sectors, so the caller places them one slot at a time rather than assuming
 * they share a buffer.
 */
static int fat32_put_slot(const struct fat32_dir_slot *slot, const struct fat32_dir_entry *entry)
{
    struct fat32_dir_entry sector[16];

    if (current_fs.dev->read_blocks(current_fs.dev, sector, slot->lba, 1) != 0) {
        return -EIO;
    }
    sector[slot->index] = *entry;
    if (current_fs.dev->write_blocks(current_fs.dev, sector, slot->lba, 1) != 0) {
        return -EIO;
    }
    return 0;
}

/*
 * fat32_clear_lfn_slots - Marks an entry's LFN entries deleted.
 *
 * Removing a name means removing all of it. The LFN entries sit immediately
 * before the 8.3 entry and still spell the old name, so an entry deleted
 * without them leaves fragments that a later scan can still assemble.
 *
 * Slots inside the caller's own sector are edited in that buffer: fat32_dir_walk
 * writes the whole sector back after the callback returns, and would otherwise
 * undo a separate write to it.
 */
static int fat32_clear_lfn_slots(const struct fat32_dir_pos *pos,
                                 struct fat32_dir_entry *sector_base)
{
    for (int k = 0; k < pos->lfn_count; k++) {
        const struct fat32_dir_slot *slot = &pos->lfn_slots[k];

        if (slot->lba == pos->lba) {
            sector_base[slot->index].name[0] = 0xE5;
            continue;
        }

        struct fat32_dir_entry sector[16];
        if (current_fs.dev->read_blocks(current_fs.dev, sector, slot->lba, 1) != 0) {
            return -EIO;
        }
        sector[slot->index].name[0] = 0xE5;
        if (current_fs.dev->write_blocks(current_fs.dev, sector, slot->lba, 1) != 0) {
            return -EIO;
        }
    }
    return 0;
}

/*
 * lfn_checksum - Ties LFN entries to the 8.3 entry they name.
 *
 * Every LFN entry carries this checksum of the short name. A tool that does
 * not understand long names can rename or replace the 8.3 entry, and the
 * mismatch is how the orphaned fragments are then recognised as stale.
 */
static uint8_t lfn_checksum(const uint8_t name[8], const uint8_t ext[3])
{
    uint8_t sum = 0;

    for (int i = 0; i < 8; i++) {
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name[i]);
    }
    for (int i = 0; i < 3; i++) {
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + ext[i]);
    }
    return sum;
}

/*
 * struct lfn_run - A long name being assembled from the fragments before an entry.
 *
 * Fragments sit in descending sequence order, the first flagged 0x40, and all
 * carry the checksum of the short name they belong to. A run that breaks any
 * of that is discarded, so a fragment a FAT tool orphaned is never read as the
 * name of whichever entry happens to follow it.
 */
struct lfn_run {
    char name[FAT32_LFN_BUF_SIZE];
    int active;
    unsigned int next_seq; // sequence the next fragment must carry; 0 once complete
    uint8_t sum;
};

static void lfn_reset(struct lfn_run *run)
{
    // extract_lfn_part writes no terminator, so a shorter name assembled next
    // would keep this one's tail.
    memset(run->name, 0, sizeof(run->name));
    run->active = 0;
    run->next_seq = 0;
}

// Returns 0 when the fragment extends the run, -1 when the run was discarded.
static int lfn_add(struct lfn_run *run, const struct fat32_lfn_entry *lfn)
{
    unsigned int seq = lfn->sequence & 0x3F;

    if (lfn->sequence & 0x40) {
        lfn_reset(run);
        run->active = 1;
        run->next_seq = seq;
        run->sum = lfn->checksum;
    }

    if (!run->active || seq != run->next_seq || lfn->checksum != run->sum
        || extract_lfn_part(lfn, run->name, sizeof(run->name)) != 0) {
        lfn_reset(run);
        return -1;
    }

    run->next_seq--;
    return 0;
}

static int lfn_names(const struct lfn_run *run, const struct fat32_dir_entry *entry)
{
    return run->active && run->next_seq == 0 && run->sum == lfn_checksum(entry->name, entry->ext);
}

// Characters FAT refuses in a short name; a long name may still contain them.
static int short_name_char_ok(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return 1;
    }
    if (c >= '0' && c <= '9') {
        return 1;
    }
    return strchr("$%'-_@~`!(){}^#&", c) != NULL;
}

/*
 * name_fits_83 - True when the name survives the trip through an 8.3 entry.
 *
 * Lowercase counts as not fitting: the 8.3 entry cannot hold it, so a name
 * typed as readme.txt needs LFN entries to come back as anything but
 * README.TXT.
 */
static int name_fits_83(const char *name)
{
    if (name[0] == '\0' || name[0] == '.') {
        return 0;
    }

    int base = 0, ext = 0, dots = 0;
    const char *p = name;

    for (; *p && *p != '.'; p++, base++) {
        if (!short_name_char_ok(*p)) {
            return 0;
        }
    }
    if (*p == '.') {
        dots++;
        for (p++; *p; p++, ext++) {
            if (*p == '.') {
                return 0; // a second dot has no 8.3 spelling
            }
            if (!short_name_char_ok(*p)) {
                return 0;
            }
        }
    }
    (void)dots;
    return base >= 1 && base <= 8 && ext <= 3;
}

struct short_name_taken_ctx {
    const uint8_t *name;
    const uint8_t *ext;
    int taken;
};

static enum fat32_walk_action short_name_taken_cb(struct fat32_dir_entry *entry,
                                                  const struct fat32_dir_pos *pos, void *ctx_,
                                                  int *out_err, int *dirty)
{
    struct short_name_taken_ctx *ctx = ctx_;
    (void)pos;
    (void)out_err;
    (void)dirty;

    if (entry->name[0] == 0x00) {
        return FAT32_WALK_STOP;
    }
    if (entry->name[0] == 0xE5) {
        return FAT32_WALK_CONTINUE;
    }

    if (memcmp(entry->name, ctx->name, 8) == 0 && memcmp(entry->ext, ctx->ext, 3) == 0) {
        ctx->taken = 1;
        return FAT32_WALK_STOP;
    }
    return FAT32_WALK_CONTINUE;
}

// Tails up to this are settled by one scan of the directory.
#define FAT32_ALIAS_SCAN 1024

// Writes BASE~N, shortening the base to make room for the tail.
static int alias_with_tail(const uint8_t *base, int base_len, uint32_t n, uint8_t out[8])
{
    char tail[8];
    int tail_len = snprintf(tail, sizeof(tail), "~%lu", (unsigned long)n);
    if (tail_len <= 0 || tail_len > 7) {
        return -1;
    }

    int keep = base_len + tail_len > 8 ? 8 - tail_len : base_len;
    memcpy(out, base, (size_t)keep);
    memcpy(out + keep, tail, (size_t)tail_len);
    memset(out + keep + tail_len, ' ', (size_t)(8 - keep - tail_len));
    return 0;
}

struct alias_scan_ctx {
    const uint8_t *base;
    int base_len;
    const uint8_t *ext;
    uint8_t taken[FAT32_ALIAS_SCAN / 8 + 1];
};

static enum fat32_walk_action alias_scan_cb(struct fat32_dir_entry *entry,
                                            const struct fat32_dir_pos *pos, void *ctx_,
                                            int *out_err, int *dirty)
{
    struct alias_scan_ctx *ctx = ctx_;
    (void)pos;
    (void)out_err;
    (void)dirty;

    if (entry->name[0] == 0x00) {
        return FAT32_WALK_STOP;
    }
    if (entry->name[0] == 0xE5 || memcmp(entry->ext, ctx->ext, 3) != 0) {
        return FAT32_WALK_CONTINUE;
    }

    int t = 0;
    while (t < 8 && entry->name[t] != '~') {
        t++;
    }
    uint32_t n = 0;
    for (int d = t + 1; d < 8 && entry->name[d] >= '0' && entry->name[d] <= '9'; d++) {
        n = n * 10 + (uint32_t)(entry->name[d] - '0');
    }
    if (t == 8 || n == 0 || n > FAT32_ALIAS_SCAN) {
        return FAT32_WALK_CONTINUE;
    }

    // The number is read off the entry; rebuilding the alias confirms the rest.
    uint8_t alias[8];
    if (alias_with_tail(ctx->base, ctx->base_len, n, alias) == 0
        && memcmp(alias, entry->name, 8) == 0) {
        ctx->taken[n / 8] |= (uint8_t)(1u << (n % 8));
    }
    return FAT32_WALK_CONTINUE;
}

/*
 * generate_short_name - Builds the 8.3 alias a long name is stored under.
 *
 * Follows the usual BASE~N.EXT shape. The tail has to make the alias unique
 * within this directory, because the short name is what every FAT tool that
 * ignores long names will see -- two files sharing one is two files that are
 * the same file.
 */
static int generate_short_name(uint32_t dir_cluster, const char *filename, uint8_t out_name[8],
                               uint8_t out_ext[3])
{
    memset(out_name, ' ', 8);
    memset(out_ext, ' ', 3);

    // Longest trailing extension, as the shell means it.
    const char *dot = NULL;
    for (const char *p = filename; *p; p++) {
        if (*p == '.') {
            dot = p;
        }
    }

    int j = 0;
    for (const char *p = filename; *p && (!dot || p < dot) && j < 6; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') {
            c -= 32;
        }
        if (c == ' ') {
            continue;
        }
        out_name[j++] = short_name_char_ok(c) ? (uint8_t)c : (uint8_t)'_';
    }
    if (j == 0) {
        out_name[j++] = '_';
    }

    if (dot) {
        int k = 0;
        for (const char *p = dot + 1; *p && k < 3; p++) {
            char c = *p;
            if (c >= 'a' && c <= 'z') {
                c -= 32;
            }
            out_ext[k++] = short_name_char_ok(c) ? (uint8_t)c : (uint8_t)'_';
        }
    }

    uint8_t base[8];
    memcpy(base, out_name, sizeof(base));

    struct alias_scan_ctx scan = {.base = base, .base_len = j, .ext = out_ext};
    memset(scan.taken, 0, sizeof(scan.taken));
    int scanned = fat32_dir_walk(dir_cluster, /*grow_chain=*/0, alias_scan_cb, &scan);
    if (scanned != 0 && scanned != -ENOENT) {
        return scanned;
    }

    for (uint32_t n = 1; n <= FAT32_ALIAS_SCAN; n++) {
        if (!(scan.taken[n / 8] & (1u << (n % 8)))) {
            return alias_with_tail(base, j, n, out_name) == 0 ? 0 : -EEXIST;
        }
    }

    // Past the scanned range each candidate costs a walk of its own.
    for (uint32_t n = FAT32_ALIAS_SCAN + 1; n < 1000000; n++) {
        if (alias_with_tail(base, j, n, out_name) != 0) {
            break;
        }

        struct short_name_taken_ctx ctx = {.name = out_name, .ext = out_ext, .taken = 0};
        int ret = fat32_dir_walk(dir_cluster, /*grow_chain=*/0, short_name_taken_cb, &ctx);
        if (ret != 0 && ret != -ENOENT) {
            return ret;
        }
        if (!ctx.taken) {
            return 0;
        }
    }
    return -EEXIST;
}

struct find_slots_ctx {
    int needed;
    int found;
    uint32_t last; // ordinal of the run's latest slot
    struct fat32_dir_slot *out;
};

/*
 * A long name's LFN entries and its 8.3 entry must be consecutive, so the
 * slots are claimed as one run. Anything from the end-of-directory marker
 * onward is free -- fat32_dir_walk zeroes a directory cluster when it grows
 * one, so there is no stale data past the marker to mistake for an entry.
 */
static enum fat32_walk_action find_slots_cb(struct fat32_dir_entry *entry,
                                            const struct fat32_dir_pos *pos, void *ctx_,
                                            int *out_err, int *dirty)
{
    struct find_slots_ctx *ctx = ctx_;
    (void)out_err;

    uint8_t first = entry->name[0];
    if (first != 0x00 && first != 0xE5) {
        ctx->found = 0; // a live entry breaks the run
        return FAT32_WALK_CONTINUE;
    }

    // Live LFN slots are never shown here, so a gap in the ordinals is one.
    if (ctx->found > 0 && pos->ordinal != ctx->last + 1) {
        ctx->found = 0;
    }
    ctx->last = pos->ordinal;

    ctx->out[ctx->found].lba = pos->lba;
    ctx->out[ctx->found].index = pos->entry_index;
    ctx->found++;

    if (ctx->found == ctx->needed) {
        return FAT32_WALK_STOP;
    }

    if (first == 0x00) {
        /*
         * Claim the marker so the walk does not stop here: everything after it
         * is free, and the run still needs more slots. Writing a deleted mark
         * is safe either way -- if the walk then fails to grow the directory,
         * the slot is simply free again.
         */
        entry->name[0] = 0xE5;
        *dirty = 1;
    }
    return FAT32_WALK_CONTINUE;
}

/*
 * fat32_write_dir_entries - Adds one name to a directory.
 *
 * A name that fits 8.3 is a single entry, as before. Anything else -- longer,
 * lowercase, or carrying characters 8.3 has no room for -- is stored as LFN
 * entries followed by a generated 8.3 alias. The LFN entries go on disk in
 * reverse order, last fragment first, which is what lets a reader assemble the
 * name having seen only the entries preceding the one it matched.
 */
static int fat32_write_dir_entries(uint32_t dir_cluster, const char *filename,
                                   struct fat32_dir_entry *short_entry)
{
    size_t len = strlen(filename);
    if (len > FAT32_LFN_MAX_NAME) {
        return -ENAMETOOLONG;
    }

    int n_lfn = 0;
    if (name_fits_83(filename)) {
        name_as_83(filename, short_entry->name, short_entry->ext);
    } else {
        int ret = generate_short_name(dir_cluster, filename, short_entry->name, short_entry->ext);
        if (ret != 0) {
            return ret;
        }
        n_lfn = (int)((len + 12) / 13);
    }

    struct fat32_dir_slot slots[FAT32_LFN_MAX_ENTRIES + 1];
    struct find_slots_ctx ctx = {.needed = n_lfn + 1, .found = 0, .out = slots};

    int ret = fat32_dir_walk(dir_cluster, /*grow_chain=*/1, find_slots_cb, &ctx);
    if (ret != 0 && ret != -ENOENT) {
        return ret;
    }
    if (ctx.found < ctx.needed) {
        return -ENOSPC;
    }

    uint8_t checksum = lfn_checksum(short_entry->name, short_entry->ext);

    // Fragment n (1-based) holds characters [(n-1)*13, n*13). On disk the
    // highest-numbered fragment comes first, so slot k holds fragment n_lfn-k.
    for (int k = 0; k < n_lfn; k++) {
        int seq = n_lfn - k;

        struct fat32_lfn_entry lfn;
        memset(&lfn, 0, sizeof(lfn));
        lfn.sequence = (uint8_t)(seq == n_lfn ? (0x40 | seq) : seq);
        lfn.attributes = 0x0F;
        lfn.type = 0;
        lfn.checksum = checksum;
        lfn.first_cluster = 0;

        uint16_t chars[13];
        for (int c = 0; c < 13; c++) {
            size_t idx = (size_t)(seq - 1) * 13 + (size_t)c;
            if (idx < len) {
                chars[c] = (uint16_t)(uint8_t)filename[idx];
            } else if (idx == len) {
                chars[c] = 0x0000; // terminator
            } else {
                chars[c] = 0xFFFF; // padding
            }
        }
        memcpy(lfn.name1, &chars[0], sizeof(lfn.name1));
        memcpy(lfn.name2, &chars[5], sizeof(lfn.name2));
        memcpy(lfn.name3, &chars[11], sizeof(lfn.name3));

        ret = fat32_put_slot(&slots[k], (struct fat32_dir_entry *)&lfn);
        if (ret != 0) {
            return ret;
        }
    }

    return fat32_put_slot(&slots[n_lfn], short_entry);
}

static int fat32_free_cluster_chain(uint32_t start_cluster)
{
    uint32_t current = start_cluster;
    while (cluster_valid(current)) {
        uint32_t next = get_next_cluster(current);
        if (set_fat_entry(current, 0) != 0) {
            return -EIO;
        }
        current = next;
    }
    return 0;
}

struct lookup_ctx {
    const char *filename;
    struct vfs_vnode *dir;
    struct vfs_vnode *result;
};

static int fat32_dir_walk(uint32_t parent_cluster, int grow_chain, fat32_dir_walk_cb cb, void *ctx_)
{
    uint32_t cluster = parent_cluster;
    struct fat32_dir_entry dirs[16];

    struct lfn_run run;
    lfn_reset(&run);

    // Where the LFN entries for the name being assembled live, so a caller
    // that removes the entry can clear them in the same pass.
    struct fat32_dir_slot lfn_slots[FAT32_LFN_MAX_ENTRIES];
    int lfn_count = 0;
    uint32_t ordinal = 0;

    // Nothing in a cluster cycle is EOC or out of range, so only a cap ends it.
    uint32_t visited = 0;

    while (cluster_valid(cluster)) {
        if (++visited > FAT32_MAX_DIR_CLUSTERS) {
            pr_err("fat32: directory chain at cluster %u does not terminate\n", parent_cluster);
            return -EIO;
        }

        uint32_t lba = cluster_to_lba(cluster);
        for (int s = 0; s < (int)current_fs.sectors_per_cluster; s++) {
            if (current_fs.dev->read_blocks(current_fs.dev, &dirs, lba + s, 1) != 0) {
                return -EIO;
            }
            for (int i = 0; i < 16; i++, ordinal++) {
                // LFN-continuation entries are consumed here and never shown
                // to cb; a deleted or end-marker byte always wins over that.
                if (dirs[i].attributes == 0x0F && dirs[i].name[0] != 0x00
                    && dirs[i].name[0] != 0xE5) {
                    struct fat32_lfn_entry *lfn = (struct fat32_lfn_entry *)&dirs[i];
                    if (lfn->sequence & 0x40) {
                        lfn_count = 0;
                    }
                    if (lfn_add(&run, lfn) != 0) {
                        lfn_count = 0;
                        continue;
                    }
                    if (lfn_count < FAT32_LFN_MAX_ENTRIES) {
                        lfn_slots[lfn_count].lba = lba + (uint32_t)s;
                        lfn_slots[lfn_count].index = i;
                        lfn_count++;
                    }
                    continue;
                }

                int named = lfn_names(&run, &dirs[i]);
                int out_err = 0;
                int dirty = 0;
                struct fat32_dir_pos pos = {
                    .lba = lba + (uint32_t)s,
                    .sector_index = s,
                    .entry_index = i,
                    .lfn_name = run.name,
                    .has_lfn = named,
                    .lfn_slots = lfn_slots,
                    .lfn_count = named ? lfn_count : 0,
                    .ordinal = ordinal,
                };
                enum fat32_walk_action action = cb(&dirs[i], &pos, ctx_, &out_err, &dirty);

                if (dirty) {
                    if (current_fs.dev->write_blocks(current_fs.dev, &dirs, lba + s, 1) != 0) {
                        return -EIO;
                    }
                }

                switch (action) {
                    case FAT32_WALK_STOP:
                        return 0;
                    case FAT32_WALK_ERROR:
                        return out_err;
                    case FAT32_WALK_CONTINUE:
                        break;
                }

                lfn_reset(&run);
                lfn_count = 0;

                // Nothing valid follows the end marker: stop the whole walk
                // right here, unless cb already claimed this slot above.
                if (dirs[i].name[0] == 0x00) {
                    return -ENOENT;
                }
            }
        }

        uint32_t next = get_next_cluster(cluster);
        if (next == FAT32_CLUSTER_IOERR) {
            return -EIO;
        }
        if (next >= FAT32_CLUSTER_EOC_MIN) {
            if (!grow_chain) {
                return -ENOENT;
            }
            next = extend_cluster_chain(cluster);
            if (next == 0) {
                return -ENOMEM;
            }
            // This walk only grows directories, and the entries below are read
            // straight out of the new cluster.
            if (zero_cluster(next) != 0) {
                return -EIO;
            }
        }
        cluster = next;
    }
    return -ENOENT;
}

static enum fat32_walk_action lookup_cb(struct fat32_dir_entry *entry,
                                        const struct fat32_dir_pos *pos, void *ctx_, int *out_err,
                                        int *dirty)
{
    struct lookup_ctx *ctx = ctx_;
    (void)pos;
    (void)dirty;

    if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) {
        return FAT32_WALK_CONTINUE;
    }
    if (!entry_named(ctx->filename, entry, pos)) {
        return FAT32_WALK_CONTINUE;
    }

    struct vfs_vnode *node = (struct vfs_vnode *)slab_alloc(sizeof(struct vfs_vnode));
    if (!node) {
        *out_err = -ENOMEM;
        return FAT32_WALK_ERROR;
    }
    memset(node, 0, sizeof(struct vfs_vnode));
    node->type = (entry->attributes & 0x10) ? VFS_VNODE_TYPE_DIR : VFS_VNODE_TYPE_REGULAR;
    node->ops = &fat32_vnode_ops;
    node->internal_info = (void *)(uintptr_t)((entry->cluster_high << 16) | entry->cluster_low);
    node->file_size = entry->size;
    node->revalidated = fat32_entry_gen;
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
    fat32_dir_walk(cluster, 0, lookup_cb, &ctx);
    return ctx.result;
}

static int fat32_vfs_readdir(struct vfs_file *file, void *buffer, size_t count)
{
    if (file->node->type != VFS_VNODE_TYPE_DIR) {
        return -ENOTDIR;
    }

    struct dirent *dirent_buf = (struct dirent *)buffer;
    size_t max_entries = count / sizeof(struct dirent);
    int entries_read = 0;

    uint32_t cluster = (uint32_t)(uintptr_t)file->node->internal_info;
    uint32_t bytes_per_cluster = current_fs.sectors_per_cluster * 512;

    if (file->offset / bytes_per_cluster > FAT32_MAX_DIR_CLUSTERS) {
        return 0;
    }

    uint32_t clusters_to_skip = (uint32_t)(file->offset / bytes_per_cluster);
    for (uint32_t i = 0; i < clusters_to_skip; i++) {
        cluster = get_next_cluster(cluster);
        if (cluster >= FAT32_CLUSTER_EOC_MIN) {
            return 0;
        }
    }

    struct lfn_run run;
    lfn_reset(&run);

    struct fat32_dir_entry dirs[16];
    uint32_t visited = 0;

    while (cluster_valid(cluster) && entries_read < (int)max_entries) {
        if (++visited > FAT32_MAX_DIR_CLUSTERS) {
            pr_err("fat32: directory chain at cluster %u does not terminate\n",
                   (uint32_t)(uintptr_t)file->node->internal_info);
            return -EIO;
        }

        uint32_t offset_in_cluster = (uint32_t)(file->offset % bytes_per_cluster);
        uint32_t start_sector = offset_in_cluster / 512;

        for (uint32_t s = start_sector;
             s < current_fs.sectors_per_cluster && entries_read < (int)max_entries; s++) {
            uint32_t lba = cluster_to_lba(cluster) + s;
            if (current_fs.dev->read_blocks(current_fs.dev, &dirs, lba, 1) != 0) {
                return -EIO;
            }

            uint32_t start_entry = (uint32_t)((file->offset % 512) / 32);
            for (int i = (int)start_entry; i < 16 && entries_read < (int)max_entries; i++) {
                file->offset += 32;

                if (dirs[i].name[0] == 0x00) {
                    return entries_read;
                }

                if (dirs[i].name[0] == 0xE5) {
                    lfn_reset(&run);
                    continue;
                }
                if (dirs[i].attributes == 0x0F) {
                    lfn_add(&run, (struct fat32_lfn_entry *)&dirs[i]);
                    continue;
                }
                // Skip . and .., and the volume label, which is not a file.
                if (dirs[i].name[0] == '.' || (dirs[i].attributes & 0x08)) {
                    lfn_reset(&run);
                    continue;
                }

                struct dirent *ent = &dirent_buf[entries_read];
                if (lfn_names(&run, &dirs[i])) {
                    strncpy(ent->d_name, run.name, 255);
                    ent->d_name[255] = '\0';
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
                        ent->d_name[idx++] = (char)c;
                    }
                    if (dirs[i].ext[0] != ' ' && dirs[i].ext[0] != 0) {
                        ent->d_name[idx++] = '.';
                        for (int k = 0; k < 3; k++) {
                            uint8_t c = dirs[i].ext[k];
                            if (c == ' ' || c == 0) {
                                continue;
                            }
                            if (c >= 'A' && c <= 'Z') {
                                c = c - 'A' + 'a';
                            }
                            ent->d_name[idx++] = (char)c;
                        }
                    }
                    ent->d_name[idx] = '\0';
                }

                lfn_reset(&run);
                ent->d_ino = (uint32_t)((dirs[i].cluster_high << 16) | dirs[i].cluster_low);
                entries_read++;
            }
        }
        cluster = get_next_cluster(cluster);
    }

    return entries_read;
}

// Stops as soon as it sees anything that isn't deleted, ".", or "..";
// fat32_dir_is_empty turns that outcome (or the lack of one) into a bool.
static enum fat32_walk_action dir_empty_cb(struct fat32_dir_entry *entry,
                                           const struct fat32_dir_pos *pos, void *ctx_,
                                           int *out_err, int *dirty)
{
    (void)pos;
    (void)ctx_;
    (void)out_err;
    (void)dirty;

    if (entry->name[0] == 0x00 || entry->name[0] == 0xE5 || entry->name[0] == '.') {
        return FAT32_WALK_CONTINUE;
    }
    return FAT32_WALK_STOP; // found a real entry: not empty
}

// Whether a directory holds anything but "." and "..", as its own small walk
// over that directory. Returns 1 if empty, 0 if not, or a negative error.
static int fat32_dir_is_empty(uint32_t cluster)
{
    int ret = fat32_dir_walk(cluster, 0, dir_empty_cb, NULL);
    if (ret == -ENOENT) {
        return 1; // walked off the end without seeing a real entry
    }
    if (ret == 0) {
        return 0; // dir_empty_cb stopped on a real entry
    }
    return ret;
}

/*
 * Removing a name and releasing its clusters are two separate on-disk writes,
 * and the order between them is what decides how a crash in the middle lands.
 *
 * Entry first, then chain: an interrupted delete leaves clusters allocated with
 * nothing referring to them -- space a scan can reclaim later.
 *
 * Chain first, then entry: an interrupted delete leaves a live-looking entry
 * pointing at clusters the allocator has already handed to the next file. Two
 * files then share data, and unlinking either frees the other's.
 *
 * So the callback never frees anything. It marks the entry and reports the
 * chain through its context; fat32_remove_entry releases it only once the walk
 * has written the entry back.
 */
struct remove_entry_ctx {
    const char *name;
    int want_dir;           // rmdir refuses a file, unlink refuses a directory
    uint32_t freed_cluster; // chain to release once the entry is on disk
};

static enum fat32_walk_action remove_entry_cb(struct fat32_dir_entry *entry,
                                              const struct fat32_dir_pos *pos, void *ctx_,
                                              int *out_err, int *dirty)
{
    struct remove_entry_ctx *ctx = ctx_;

    if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) {
        return FAT32_WALK_CONTINUE;
    }
    if (!entry_named(ctx->name, entry, pos)) {
        return FAT32_WALK_CONTINUE;
    }

    int is_dir = (entry->attributes & 0x10) != 0;
    if (ctx->want_dir && !is_dir) {
        *out_err = -ENOTDIR;
        return FAT32_WALK_ERROR;
    }
    if (!ctx->want_dir && is_dir) {
        *out_err = -EISDIR;
        return FAT32_WALK_ERROR;
    }

    uint32_t target_cluster = ((uint32_t)entry->cluster_high << 16) | entry->cluster_low;

    if (ctx->want_dir) {
        int empty = fat32_dir_is_empty(target_cluster);
        if (empty < 0) {
            *out_err = empty;
            return FAT32_WALK_ERROR;
        }
        if (!empty) {
            *out_err = -ENOTEMPTY;
            return FAT32_WALK_ERROR;
        }
    }

    // entry points into the walk's sector buffer; back up to its first entry.
    int lfn_err = fat32_clear_lfn_slots(pos, entry - pos->entry_index);
    if (lfn_err != 0) {
        *out_err = lfn_err;
        return FAT32_WALK_ERROR;
    }

    entry->name[0] = 0xE5;
    *dirty = 1;
    ctx->freed_cluster = target_cluster;
    return FAT32_WALK_STOP;
}

static int fat32_remove_entry(uint32_t parent_cluster, const char *name, int want_dir)
{
    struct remove_entry_ctx ctx = {.name = name, .want_dir = want_dir, .freed_cluster = 0};

    int ret = fat32_dir_walk(parent_cluster, 0, remove_entry_cb, &ctx);
    if (ret != 0) {
        return ret;
    }

    if (ctx.freed_cluster >= 2) {
        // Keyed on the start cluster, so the next file to get it inherits them.
        pagecache_discard(&fat32_vnode_ops, (void *)(uintptr_t)ctx.freed_cluster);
        return fat32_free_cluster_chain(ctx.freed_cluster);
    }
    return 0;
}

static int fat32_unlink(struct vfs_vnode *parent, const char *name)
{
    return fat32_remove_entry((uint32_t)(uintptr_t)parent->internal_info, name, 0);
}

static int fat32_rmdir(struct vfs_vnode *parent, const char *name)
{
    return fat32_remove_entry((uint32_t)(uintptr_t)parent->internal_info, name, 1);
}

static int fat32_mkdir(struct vfs_vnode *parent, const char *name)
{
    if (strlen(name) > 255) {
        return -EINVAL;
    }

    struct vfs_vnode *existing = fat32_vfs_lookup(parent, name);
    if (existing) {
        vfs_vnode_put(existing);
        return -EEXIST;
    }

    uint32_t new_cluster = allocate_cluster();
    if (new_cluster == 0) {
        return -ENOMEM;
    }

    // Nothing on disk refers to the cluster until the entry lands, so every
    // failure below has to free it.
    if (zero_cluster(new_cluster) != 0) {
        fat32_free_cluster_chain(new_cluster);
        return -EIO;
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
    uint32_t dotdot = dotdot_cluster(parent_cluster);
    sector[1].cluster_high = dotdot >> 16;
    sector[1].cluster_low = dotdot & 0xFFFF;

    if (current_fs.dev->write_blocks(current_fs.dev, sector, cluster_to_lba(new_cluster), 1) != 0) {
        fat32_free_cluster_chain(new_cluster);
        return -EIO;
    }

    struct fat32_dir_entry new_entry;
    memset(&new_entry, 0, sizeof(new_entry));
    new_entry.attributes = 0x10;
    new_entry.cluster_high = new_cluster >> 16;
    new_entry.cluster_low = new_cluster & 0xFFFF;
    new_entry.size = 0;
    fat32_stamp_entry(&new_entry, /*created=*/1);

    int err = fat32_write_dir_entries(parent_cluster, name, &new_entry);
    if (err != 0) {
        fat32_free_cluster_chain(new_cluster);
    }
    return err;
}

struct rename_find_ctx {
    const char *name;
    struct fat32_dir_entry entry;
    struct fat32_dir_slot slot;

    // Copied out of the walk, whose array is gone once it returns.
    struct fat32_dir_slot lfn_slots[FAT32_LFN_MAX_ENTRIES];
    int lfn_count;
};

static enum fat32_walk_action rename_find_cb(struct fat32_dir_entry *entry,
                                             const struct fat32_dir_pos *pos, void *ctx_,
                                             int *out_err, int *dirty)
{
    struct rename_find_ctx *ctx = ctx_;
    (void)out_err;
    (void)dirty;

    if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) {
        return FAT32_WALK_CONTINUE;
    }
    if (!entry_named(ctx->name, entry, pos)) {
        return FAT32_WALK_CONTINUE;
    }

    ctx->entry = *entry;
    ctx->slot.lba = pos->lba;
    ctx->slot.index = pos->entry_index;

    ctx->lfn_count = pos->lfn_count;
    for (int k = 0; k < pos->lfn_count; k++) {
        ctx->lfn_slots[k] = pos->lfn_slots[k];
    }
    return FAT32_WALK_STOP;
}

// A directory's ".." is its second slot.
static int fat32_set_dotdot(uint32_t dir_cluster, uint32_t parent_cluster)
{
    struct fat32_dir_entry sector[16];
    uint32_t lba = cluster_to_lba(dir_cluster);

    if (current_fs.dev->read_blocks(current_fs.dev, sector, lba, 1) != 0) {
        return -EIO;
    }
    if (sector[1].name[0] != '.' || sector[1].name[1] != '.') {
        return 0;
    }

    uint32_t dotdot = dotdot_cluster(parent_cluster);
    sector[1].cluster_high = (uint16_t)(dotdot >> 16);
    sector[1].cluster_low = (uint16_t)(dotdot & 0xFFFF);
    return current_fs.dev->write_blocks(current_fs.dev, sector, lba, 1) != 0 ? -EIO : 0;
}

static int fat32_rename(struct vfs_vnode *old_parent, const char *old_name,
                        struct vfs_vnode *new_parent, const char *new_name)
{
    uint32_t old_cluster = (uint32_t)(uintptr_t)old_parent->internal_info;
    struct rename_find_ctx ctx = {.name = old_name};
    int found = fat32_dir_walk(old_cluster, 0, rename_find_cb, &ctx);
    if (found != 0) {
        return found;
    }

    uint32_t new_parent_cluster = (uint32_t)(uintptr_t)new_parent->internal_info;
    int is_dir = (ctx.entry.attributes & 0x10) != 0;

    // An existing target is replaced once the kinds agree. Finding the entry
    // being renamed is a change of case, not a target.
    struct rename_find_ctx target = {.name = new_name};
    int t = fat32_dir_walk(new_parent_cluster, 0, rename_find_cb, &target);
    if (t != 0 && t != -ENOENT) {
        return t;
    }

    uint32_t replaced = 0; // the target's chain, freed once no entry names it

    if (t == 0 && (target.slot.lba != ctx.slot.lba || target.slot.index != ctx.slot.index)) {
        int target_is_dir = (target.entry.attributes & 0x10) != 0;
        if (is_dir != target_is_dir) {
            return is_dir ? -ENOTDIR : -EISDIR;
        }

        uint32_t target_cluster =
            ((uint32_t)target.entry.cluster_high << 16) | (uint32_t)target.entry.cluster_low;
        if (target_is_dir) {
            int empty = fat32_dir_is_empty(target_cluster);
            if (empty <= 0) {
                return empty < 0 ? empty : -ENOTEMPTY;
            }
        }

        // The source takes over the target's entry, name and all, so there is
        // no moment at which the target name is missing and no slot to find.
        struct fat32_dir_entry sector[16];
        if (current_fs.dev->read_blocks(current_fs.dev, sector, target.slot.lba, 1) != 0) {
            return -EIO;
        }
        struct fat32_dir_entry taken = ctx.entry;
        memcpy(taken.name, target.entry.name, sizeof(taken.name));
        memcpy(taken.ext, target.entry.ext, sizeof(taken.ext));
        sector[target.slot.index] = taken;
        if (current_fs.dev->write_blocks(current_fs.dev, sector, target.slot.lba, 1) != 0) {
            return -EIO;
        }

        uint32_t source_cluster =
            ((uint32_t)ctx.entry.cluster_high << 16) | (uint32_t)ctx.entry.cluster_low;
        if (target_cluster != source_cluster) {
            replaced = target_cluster;
        }
    } else {
        struct fat32_dir_entry new_entry = ctx.entry;
        int res = fat32_write_dir_entries(new_parent_cluster, new_name, &new_entry);
        if (res != 0) {
            return res;
        }
    }

    /*
     * The new name is on disk now, so the old one can go. Deleting it second
     * is deliberate: a crash between the two leaves the file reachable under
     * both names, where the other order would leave it reachable under
     * neither.
     *
     * Re-read the slot rather than trusting a buffer from the earlier walk --
     * the insert above may have grown or rewritten the directory.
     */
    struct fat32_dir_entry dirs[16];
    if (current_fs.dev->read_blocks(current_fs.dev, &dirs, ctx.slot.lba, 1) != 0) {
        return -EIO;
    }
    dirs[ctx.slot.index].name[0] = 0xE5;
    if (current_fs.dev->write_blocks(current_fs.dev, &dirs, ctx.slot.lba, 1) != 0) {
        return -EIO;
    }

    // The old name's LFN entries spell the old name and must go with it.
    for (int k = 0; k < ctx.lfn_count; k++) {
        struct fat32_dir_entry sector[16];
        if (current_fs.dev->read_blocks(current_fs.dev, sector, ctx.lfn_slots[k].lba, 1) != 0) {
            return -EIO;
        }
        sector[ctx.lfn_slots[k].index].name[0] = 0xE5;
        if (current_fs.dev->write_blocks(current_fs.dev, sector, ctx.lfn_slots[k].lba, 1) != 0) {
            return -EIO;
        }
    }

    // Last, so a crash before this leaks the chain rather than cross-links it.
    if (replaced >= 2) {
        pagecache_discard(&fat32_vnode_ops, (void *)(uintptr_t)replaced);
        fat32_free_cluster_chain(replaced);
    }

    if (is_dir && new_parent_cluster != old_cluster) {
        uint32_t dir_cluster =
            ((uint32_t)ctx.entry.cluster_high << 16) | (uint32_t)ctx.entry.cluster_low;
        return fat32_set_dotdot(dir_cluster, new_parent_cluster);
    }
    return 0;
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
        return -ENOSPC;
    }

    // Zero the cluster so stale data from a deleted file cannot surface.
    if (zero_cluster(new_cluster) != 0) {
        fat32_free_cluster_chain(new_cluster);
        return -EIO;
    }

    struct fat32_dir_entry new_entry;
    memset(&new_entry, 0, sizeof(new_entry));
    new_entry.attributes = 0x20; // archive = regular file
    new_entry.cluster_high = (uint16_t)(new_cluster >> 16);
    new_entry.cluster_low = (uint16_t)(new_cluster & 0xFFFF);
    new_entry.size = 0;
    fat32_stamp_entry(&new_entry, /*created=*/1);

    uint32_t parent_cluster = (uint32_t)(uintptr_t)parent->internal_info;
    int res = fat32_write_dir_entries(parent_cluster, name, &new_entry);
    if (res != 0) {
        fat32_free_cluster_chain(new_cluster);
    }
    return res;
}

struct stat_ctx {
    const char *name;
    uint16_t mod_date;
    uint16_t mod_time;
    uint16_t create_date;
    uint16_t create_time;
    int found;
};

static enum fat32_walk_action stat_cb(struct fat32_dir_entry *entry,
                                      const struct fat32_dir_pos *pos, void *ctx_, int *out_err,
                                      int *dirty)
{
    struct stat_ctx *ctx = ctx_;
    (void)out_err;
    (void)dirty;

    if (entry->name[0] == 0x00) {
        return FAT32_WALK_STOP;
    }
    if (entry->name[0] == 0xE5) {
        return FAT32_WALK_CONTINUE;
    }
    if (!entry_named(ctx->name, entry, pos)) {
        return FAT32_WALK_CONTINUE;
    }

    ctx->mod_date = entry->last_mod_date;
    ctx->mod_time = entry->last_mod_time;
    ctx->create_date = entry->create_date;
    ctx->create_time = entry->create_time;
    ctx->found = 1;
    return FAT32_WALK_STOP;
}

// Unpacks a FAT date/time pair into seconds since the Unix epoch.
static time_t fat32_decode_time(uint16_t date, uint16_t time)
{
    if (date == 0) {
        return 0; // never stamped
    }

    int year = 1980 + (date >> 9);
    int month = (date >> 5) & 0x0F;
    int day = date & 0x1F;
    int hour = time >> 11;
    int min = (time >> 5) & 0x3F;
    int sec = (time & 0x1F) * 2;

    if (month < 1 || month > 12 || day < 1 || day > 31) {
        return 0;
    }
    return timer_civil_to_epoch(year, month, day, hour, min, sec);
}

/*
 * fat32_stat - Adds the times to what the VFS already filled in.
 *
 * They live in the directory entry rather than on the vnode, so this re-reads
 * the parent. FAT has no change-time, so st_ctime reports creation, which is
 * what the field means on the systems this format came from.
 */
static int fat32_stat(struct vfs_vnode *node, struct stat *buf)
{
    if (!node->parent) {
        return 0; // the root has no entry naming it
    }

    uint32_t parent_cluster = (uint32_t)(uintptr_t)node->parent->internal_info;
    struct stat_ctx ctx = {.name = node->name, .found = 0};

    int ret = fat32_dir_walk(parent_cluster, /*grow_chain=*/0, stat_cb, &ctx);
    if (ret != 0 && ret != -ENOENT) {
        return ret;
    }
    if (!ctx.found) {
        return 0;
    }

    buf->st_mtime = (uint32_t)fat32_decode_time(ctx.mod_date, ctx.mod_time);
    buf->st_ctime = (uint32_t)fat32_decode_time(ctx.create_date, ctx.create_time);
    buf->st_atime = buf->st_mtime;
    return 0;
}

static int fat32_op_stat(struct vfs_vnode *node, struct stat *buf)
{
    kmutex_lock(&fat32_lock);
    int r = fat32_stat(node, buf);
    kmutex_unlock(&fat32_lock);
    return r;
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
    file->node->revalidated = ++fat32_entry_gen;
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
    fat32_entry_gen++;
    kmutex_unlock(&fat32_lock);
    return r;
}

static int fat32_op_mkdir(struct vfs_vnode *parent, const char *name)
{
    kmutex_lock(&fat32_lock);
    int r = fat32_mkdir(parent, name);
    fat32_entry_gen++;
    kmutex_unlock(&fat32_lock);
    return r;
}

static int fat32_op_create(struct vfs_vnode *parent, const char *name)
{
    kmutex_lock(&fat32_lock);
    int r = fat32_create(parent, name);
    fat32_entry_gen++;
    kmutex_unlock(&fat32_lock);
    return r;
}

static int fat32_op_rmdir(struct vfs_vnode *parent, const char *name)
{
    kmutex_lock(&fat32_lock);
    int r = fat32_rmdir(parent, name);
    fat32_entry_gen++;
    kmutex_unlock(&fat32_lock);
    return r;
}

static int fat32_op_unlink(struct vfs_vnode *parent, const char *name)
{
    kmutex_lock(&fat32_lock);
    int r = fat32_unlink(parent, name);
    fat32_entry_gen++;
    kmutex_unlock(&fat32_lock);
    return r;
}

static int fat32_op_rename(struct vfs_vnode *old_parent, const char *old_name,
                           struct vfs_vnode *new_parent, const char *new_name)
{
    kmutex_lock(&fat32_lock);
    int r = fat32_rename(old_parent, old_name, new_parent, new_name);
    fat32_entry_gen++;
    kmutex_unlock(&fat32_lock);
    return r;
}

static int fat32_truncate(struct vfs_vnode *node, vfs_off_t length)
{
    if (length > node->file_size) {
        return -ENOTSUP; // grow: TODO, first cut
    }
    if (length == node->file_size) {
        return 0; // no-op
    }

    pagecache_invalidate(node);

    /*
     * Same ordering rule as fat32_remove_entry: shrink the entry before the
     * chain, never the reverse.
     */
    uint32_t start_cluster = (uint32_t)(uintptr_t)node->internal_info;
    uint32_t new_tail = 0; // cluster to cap with an end-of-chain marker
    uint32_t doomed = 0;   // first cluster of the chain to release

    void *old_internal = node->internal_info;
    vfs_off_t old_size = node->file_size;

    if (length == 0) {
        if (cluster_valid(start_cluster)) {
            doomed = start_cluster;
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
            new_tail = last_keep;
            uint32_t next = get_next_cluster(last_keep);
            if (cluster_valid(next)) {
                doomed = next;
            }
        }
    }

    node->file_size = length;

    /*
     * If the entry cannot be updated, the clusters stay where they are: the
     * on-disk entry still describes the old length and must keep referring to
     * a chain that covers it.
     */
    int err = fat32_update_dir_entry(node);
    if (err != 0) {
        // The vnode goes back too, or the next write allocates a second chain
        // and orphans the one the entry still names.
        node->internal_info = old_internal;
        node->file_size = old_size;
        return err;
    }

    if (new_tail) {
        set_fat_entry(new_tail, FAT32_CLUSTER_EOC);
    }
    if (doomed) {
        fat32_free_cluster_chain(doomed);
    }
    return 0;
}

static int fat32_op_truncate(struct vfs_vnode *node, vfs_off_t length)
{
    kmutex_lock(&fat32_lock);
    int r = fat32_truncate(node, length);
    node->revalidated = ++fat32_entry_gen;
    kmutex_unlock(&fat32_lock);
    return r;
}

static int fat32_op_revalidate(struct vfs_vnode *node)
{
    kmutex_lock(&fat32_lock);
    int r = fat32_revalidate(node);
    kmutex_unlock(&fat32_lock);
    return r;
}

static struct vfs_vnode_ops fat32_vnode_ops = {
    .read = fat32_op_read,
    .write = fat32_op_write,
    .revalidate = fat32_op_revalidate,
    .truncate = fat32_op_truncate,
    .stat = fat32_op_stat,
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
        return -ENOTSUP;
    }

    /* A power of two up to 128 keeps a cluster within the 64 KB the spec allows
     * and, more importantly here, keeps it non-zero: it is a divisor. */
    if (spc == 0 || spc > 128 || (spc & (spc - 1)) != 0) {
        return -EINVAL;
    }
    if (reserved == 0 || sectors_per_fat == 0 || num_fats < 1 || num_fats > 2) {
        return -EINVAL;
    }
    if (bpb->root_cluster < 2) {
        return -EINVAL;
    }

    /* Compute in 64 bits: num_fats * sectors_per_fat overflows a uint32_t for
     * plausible-looking values, wrapping data_lba_start back over the FAT. */
    uint64_t fat_lba = (uint64_t)partition_lba + reserved;
    uint64_t data_lba = fat_lba + (uint64_t)num_fats * sectors_per_fat;

    if (data_lba >= device_blocks) {
        return -EINVAL;
    }

    uint64_t cluster_count = (device_blocks - data_lba) / spc;
    if (cluster_count == 0) {
        return -EINVAL;
    }

    // Clusters are numbered from 2, and the top values are reserved markers.
    uint64_t max_cluster = cluster_count + 1;
    if (max_cluster > FAT32_CLUSTER_MAX) {
        max_cluster = FAT32_CLUSTER_MAX;
    }

    // The FAT is the real limit at 128 entries per sector, and a BPB can
    // describe a data area larger than it addresses.
    uint64_t fat_capacity = (uint64_t)sectors_per_fat * (FAT32_SECTOR_SIZE / 4);
    if (max_cluster >= fat_capacity) {
        max_cluster = fat_capacity - 1;
    }
    if (max_cluster < 2) {
        return -EINVAL;
    }

    if (bpb->root_cluster > max_cluster) {
        return -EINVAL;
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

    return 0;
}

/*
 * fat32_read_volume - Finds the FAT32 volume on a device and derives its geometry.
 */
static int fat32_read_volume(struct block_device *dev, struct fat32_fs *out)
{
    uint8_t sector0[512];
    if (dev->read_blocks(dev, sector0, 0, 1) != 0) {
        return -EIO;
    }

    uint16_t sig = *(uint16_t *)(sector0 + 510);
    if (sig != 0xAA55) {
        return -EINVAL;
    }

    uint32_t partition_lba = 0;
    if (sector0[0] != 0xEB && sector0[0] != 0xE9) {
        struct mbr *mbr = (struct mbr *)sector0;
        int found = 0;
        for (size_t i = 0; i < 4 && !found; i++) {
            if (mbr->partitions[i].type == 0x0B || mbr->partitions[i].type == 0x0C) {
                partition_lba = mbr->partitions[i].lba_start;
                found = 1;
            }
        }
        if (!found) {
            return -ENOENT;
        }
    }

    struct fat32_bpb bpb;
    if (dev->read_blocks(dev, &bpb, partition_lba, 1) != 0) {
        return -EIO;
    }

    int geom = fat32_geometry_from_bpb(&bpb, partition_lba, (uint64_t)dev->block_count, out);
    if (geom != 0) {
        pr_err("fat32: rejecting volume with an implausible BPB\n");
        return geom;
    }

    out->dev = dev;
    return 0;
}

#ifdef CONFIG_TESTS
int fat32_test_geometry_from_bpb(const struct fat32_bpb *bpb, uint32_t partition_lba,
                                 uint64_t device_blocks, struct fat32_fs *out)
{
    return fat32_geometry_from_bpb(bpb, partition_lba, device_blocks, out);
}

int fat32_test_dotdot(struct vfs_vnode *dir, uint32_t *out)
{
    struct fat32_dir_entry sector[16];

    kmutex_lock(&fat32_lock);
    uint32_t lba = cluster_to_lba((uint32_t)(uintptr_t)dir->internal_info);
    int r = current_fs.dev->read_blocks(current_fs.dev, sector, lba, 1);
    kmutex_unlock(&fat32_lock);

    if (r != 0) {
        return -EIO;
    }
    *out = ((uint32_t)sector[1].cluster_high << 16) | sector[1].cluster_low;
    return 0;
}

int fat32_test_read_volume(struct block_device *dev)
{
    struct fat32_fs fs = {0};
    return fat32_read_volume(dev, &fs);
}

int fat32_test_scan_root(struct block_device *dev, const char *name, int *found, int *readdir_ret)
{
    struct fat32_fs fs = {0};
    int err = fat32_read_volume(dev, &fs);
    if (err != 0) {
        return err;
    }

    kmutex_lock(&fat32_lock);
    struct fat32_fs live = current_fs;
    current_fs = fs;

    struct vfs_vnode *root = fat32_get_root_node();
    if (root) {
        struct vfs_vnode *node = fat32_vfs_lookup(root, name);
        *found = node != NULL;
        if (node) {
            vfs_vnode_put(node);
        }

        struct dirent ents[2];
        struct vfs_file file = {.node = root};
        *readdir_ret = fat32_vfs_readdir(&file, ents, sizeof(ents));
        vfs_vnode_put(root);
    } else {
        err = -ENOMEM;
    }

    current_fs = live;
    kmutex_unlock(&fat32_lock);
    return err;
}
#endif

int fat32_init(const char *device_name)
{
    struct block_device *dev = block_device_lookup(device_name);
    if (!dev) {
        return -ENOENT;
    }

    // Validated whole before it replaces anything, so a rejected volume leaves
    // the mounted one intact.
    struct fat32_fs fs = {0};
    int err = fat32_read_volume(dev, &fs);
    if (err != 0) {
        return err;
    }

    kmutex_lock(&fat32_lock);
    current_fs = fs;
    kmutex_unlock(&fat32_lock);

    pr_info("fat32: partition at LBA %u, %u clusters of %u sectors\n", fs.partition_lba_start,
            fs.max_cluster - 1, fs.sectors_per_cluster);
    return 0;
}
