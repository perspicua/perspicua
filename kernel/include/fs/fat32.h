#ifndef PERSPICUA_KERNEL_FAT32_H
#define PERSPICUA_KERNEL_FAT32_H

#include "types.h"
#include "driver/block.h"
#include "fs/vfs.h"

struct partition_entry
{
    uint8_t boot_flag;
    uint8_t chs_start[3];
    uint8_t type;
    uint8_t chs_end[3];
    uint32_t lba_start;
    uint32_t num_sectors;
} __attribute__((packed));

struct mbr
{
    uint8_t bootstrap[446];
    struct partition_entry partitions[4];
    uint16_t signature;
} __attribute__((packed));

struct fat32_bpb
{
    uint8_t jmp[3];
    uint8_t oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t media_type;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t sectors_per_fat_32;
    uint16_t flags;
    uint16_t fat_version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    uint8_t volume_label[11];
    uint8_t system_id[8];
} __attribute__((packed));

struct fat32_dir_entry
{
    uint8_t name[8];
    uint8_t ext[3];
    uint8_t attributes;
    uint8_t reserved;
    uint8_t create_time_ms;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t last_access_date;
    uint16_t cluster_high;
    uint16_t last_mod_time;
    uint16_t last_mod_date;
    uint16_t cluster_low;
    uint32_t size;
} __attribute__((packed));

struct fat32_fs
{
    struct block_device* dev;
    uint32_t partition_lba_start;

    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t num_fats;
    uint32_t sectors_per_fat;
    uint32_t root_cluster;

    uint32_t fat_lba_start;
    uint32_t data_lba_start;
};

int fat32_init(const char* device_name);
void fat32_ls();
void fat32_cat(const char* filename);

// VFS Interface
struct vfs_vnode* fat32_get_root_node(void);
int fat32_vfs_read(struct vfs_file* file, void* buffer, size_t size);
struct vfs_vnode* fat32_vfs_lookup(struct vfs_vnode* dir, const char* filename);

#endif  // PERSPICUA_KERNEL_FAT32_H
