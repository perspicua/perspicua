#include "fs/fat32.h"
#include "uapi/errors.h"
#include "stdio.h"
#include "mm/slab.h"
#include "string.h"
#include "core/lock.h"
static struct fat32_fs current_fs;

static uint32_t cluster_to_lba(uint32_t cluster)
{
    return current_fs.data_lba_start + (cluster - 2) * current_fs.sectors_per_cluster;
}

static uint32_t get_next_cluster(uint32_t cluster)
{
    uint32_t fat_sector = current_fs.fat_lba_start + (cluster / 128);
    uint32_t fat_offset = cluster % 128;
    uint32_t fat_buffer[128];

    if (current_fs.dev->read_blocks(current_fs.dev, &fat_buffer, fat_sector, 1) != 0)
    {
        return 0x0FFFFFFF;
    }

    return fat_buffer[fat_offset] & 0x0FFFFFFF;
}

static int name_match(const char* filename, struct fat32_dir_entry* entry)
{
    char name[8], ext[3];
    int i = 0, j = 0;

    for (int k = 0; k < 8; k++)
        name[k] = ' ';
    for (int k = 0; k < 3; k++)
        ext[k] = ' ';

    while (filename[i] != '.' && filename[i] != '\0' && j < 8)
    {
        char c = filename[i++];
        if (c >= 'a' && c <= 'z')
            c -= 32;
        name[j++] = c;
    }

    if (filename[i] == '.')
    {
        i++;
        j = 0;
        while (filename[i] != '\0' && j < 3)
        {
            char c = filename[i++];
            if (c >= 'a' && c <= 'z')
                c -= 32;
            ext[j++] = c;
        }
    }

    for (int k = 0; k < 8; k++)
        if (name[k] != entry->name[k])
            return 0;
    for (int k = 0; k < 3; k++)
        if (ext[k] != entry->ext[k])
            return 0;
    return 1;
}

int fat32_vfs_read(struct vfs_file* file, void* buffer, size_t size)
{
    if (!file || !file->node || !buffer)
        return -PERS_ERR_INVALID_ARGUMENT;

    uint32_t cluster = (uint32_t)(uintptr_t)file->node->internal_info;
    uint32_t file_size = (uint32_t)file->node->file_size;
    uint32_t offset = (uint32_t)file->offset;

    if (offset >= file_size)
        return 0;
    if (offset + size > file_size)
        size = file_size - offset;

    uint32_t bytes_per_cluster = current_fs.sectors_per_cluster * 512;

    uint32_t clusters_to_skip = offset / bytes_per_cluster;
    for (uint32_t i = 0; i < clusters_to_skip; i++)
    {
        cluster = get_next_cluster(cluster);
        if (cluster >= 0x0FFFFFF8)
            return 0;
    }

    uint32_t bytes_read = 0;
    uint8_t sector_buffer[512];

    while (bytes_read < size)
    {
        uint32_t offset_in_cluster = (offset + bytes_read) % bytes_per_cluster;
        uint32_t sector_in_cluster = offset_in_cluster / 512;
        uint32_t offset_in_sector = offset_in_cluster % 512;

        uint32_t lba = cluster_to_lba(cluster) + sector_in_cluster;

        if (current_fs.dev->read_blocks(current_fs.dev, sector_buffer, lba, 1) != 0)
        {
            break;
        }

        uint32_t can_read = 512 - offset_in_sector;
        uint32_t remaining = size - bytes_read;
        uint32_t to_copy = (can_read < remaining) ? can_read : remaining;

        for (uint32_t i = 0; i < to_copy; i++)
        {
            ((uint8_t*)buffer)[bytes_read + i] = sector_buffer[offset_in_sector + i];
        }

        bytes_read += to_copy;

        if ((offset + bytes_read) % bytes_per_cluster == 0)
        {
            cluster = get_next_cluster(cluster);
            if (cluster >= 0x0FFFFFF8 && bytes_read < size)
                break;
        }
    }

    file->offset += bytes_read;
    return (int)bytes_read;
}

static struct vfs_vnode_ops fat32_vnode_ops;

struct vfs_vnode* fat32_vfs_lookup(struct vfs_vnode* dir, const char* filename)
{
    uint32_t cluster = (uint32_t)(uintptr_t)dir->internal_info;
    struct fat32_dir_entry dirs[16];

    while (cluster < 0x0FFFFFF8)
    {
        uint32_t lba = cluster_to_lba(cluster);
        for (int s = 0; s < (int)current_fs.sectors_per_cluster; s++)
        {
            if (current_fs.dev->read_blocks(current_fs.dev, &dirs, lba + s, 1) != 0)
                return NULL;
            for (int i = 0; i < 16; i++)
            {
                if (dirs[i].name[0] == 0x00)
                    return NULL;
                if (dirs[i].name[0] == 0xE5)
                    continue;
                if (name_match(filename, &dirs[i]))
                {
                    struct vfs_vnode* node = (struct vfs_vnode*)slab_alloc(sizeof(struct vfs_vnode));
                    if (!node)
                        return NULL;
                    memset(node, 0, sizeof(struct vfs_vnode));
                    node->type = (dirs[i].attributes & 0x10) ? VFS_VNODE_TYPE_DIR : VFS_VNODE_TYPE_REGULAR;
                    node->ops = &fat32_vnode_ops;
                    node->internal_info = (void*)(uintptr_t)((dirs[i].cluster_high << 16) | dirs[i].cluster_low);
                    node->file_size = dirs[i].size;
                    node->parent = dir;
                    atomic_inc(&dir->refcount);
                    node->refcount.counter = 1;
                    return node;
                }
            }
        }
        cluster = get_next_cluster(cluster);
    }
    return NULL;
}

int fat32_vfs_readdir(struct vfs_file* file, void* buffer, size_t count)
{
    if (file->node->type != VFS_VNODE_TYPE_DIR)
        return -PERS_ERR_NOT_A_DIRECTORY;

    struct vfs_dirent* dirent_buf = (struct vfs_dirent*)buffer;
    size_t max_entries = count / sizeof(struct vfs_dirent);
    int entries_read = 0;

    uint32_t cluster = (uint32_t)(uintptr_t)file->node->internal_info;
    uint32_t bytes_per_cluster = current_fs.sectors_per_cluster * 512;

    /* Treat file->offset as a raw byte offset into the directory (32-byte entries). */
    uint32_t clusters_to_skip = (uint32_t)(file->offset / bytes_per_cluster);
    for (uint32_t i = 0; i < clusters_to_skip; i++)
    {
        cluster = get_next_cluster(cluster);
        if (cluster >= 0x0FFFFFF8)
            return 0;
    }

    struct fat32_dir_entry dirs[16];
    while (cluster < 0x0FFFFFF8 && entries_read < (int)max_entries)
    {
        uint32_t offset_in_cluster = (uint32_t)(file->offset % bytes_per_cluster);
        uint32_t start_sector = offset_in_cluster / 512;

        for (uint32_t s = start_sector; s < current_fs.sectors_per_cluster && entries_read < (int)max_entries; s++)
        {
            uint32_t lba = cluster_to_lba(cluster) + s;
            if (current_fs.dev->read_blocks(current_fs.dev, &dirs, lba, 1) != 0)
                return -PERS_ERR_IO_ERROR;

            uint32_t start_entry = (uint32_t)((file->offset % 512) / 32);
            for (int i = (int)start_entry; i < 16 && entries_read < (int)max_entries; i++)
            {
                /* Advance offset immediately so we don't re-process this entry next time */
                file->offset += 32;

                if (dirs[i].name[0] == 0x00)
                    return entries_read; /* End of directory */
                if (dirs[i].name[0] == 0xE5 || dirs[i].attributes == 0x0F)
                    continue; /* Deleted or LFN entry */

                struct vfs_dirent* dirent = &dirent_buf[entries_read];
                int idx = 0;
                for (int k = 0; k < 8; k++)
                {
                    uint8_t c = dirs[i].name[k];
                    if (c == ' ' || c == 0)
                        continue;
                    if (c >= 'A' && c <= 'Z')
                        c = c - 'A' + 'a';
                    dirent->name[idx++] = (char)c;
                }
                if (dirs[i].ext[0] != ' ' && dirs[i].ext[0] != 0)
                {
                    dirent->name[idx++] = '.';
                    for (int k = 0; k < 3; k++)
                    {
                        uint8_t c = dirs[i].ext[k];
                        if (c == ' ' || c == 0)
                            continue;
                        if (c >= 'A' && c <= 'Z')
                            c = c - 'A' + 'a';
                        dirent->name[idx++] = (char)c;
                    }
                }
                dirent->name[idx] = '\0';
                dirent->ino = (uint32_t)((dirs[i].cluster_high << 16) | dirs[i].cluster_low);
                entries_read++;
            }
        }
        cluster = get_next_cluster(cluster);
    }

    return entries_read;
}

static struct vfs_vnode_ops fat32_vnode_ops = {
    .read = fat32_vfs_read,
    .lookup = fat32_vfs_lookup,
    .readdir = fat32_vfs_readdir,
};

struct vfs_vnode* fat32_get_root_node(void)
{
    struct vfs_vnode* node = slab_alloc(sizeof(struct vfs_vnode));
    if (!node)
        return NULL;
    node->type = VFS_VNODE_TYPE_DIR;
    node->ops = &fat32_vnode_ops;
    node->internal_info = (void*)(uintptr_t)current_fs.root_cluster;
    node->file_size = 0;
    return node;
}

static void print_fat_name(const uint8_t* name, const uint8_t* ext)
{
    for (int i = 0; i < 8; i++)
    {
        if (name[i] != ' ' && name[i] != 0)
            printk("%c", name[i]);
    }
    if (ext[0] != ' ' && ext[0] != 0)
    {
        printk(".");
        for (int i = 0; i < 3; i++)
        {
            if (ext[i] != ' ' && ext[i] != 0)
                printk("%c", ext[i]);
        }
    }
}

void fat32_ls(void)
{
    uint32_t lba = cluster_to_lba(current_fs.root_cluster);
    struct fat32_dir_entry dirs[16];
    if (current_fs.dev->read_blocks(current_fs.dev, &dirs, lba, 1) != 0)
        return;
    for (size_t i = 0; i < 16; i++)
    {
        if (dirs[i].name[0] == 0x00)
            break;
        if (dirs[i].name[0] == 0xE5 || dirs[i].attributes == 0x0F)
            continue;
        printk("fat32: ");
        print_fat_name(dirs[i].name, dirs[i].ext);
        printk("  (size: %u bytes)\n", dirs[i].size);
    }
}

void fat32_cat(const char* filename)
{
    uint32_t lba = cluster_to_lba(current_fs.root_cluster);
    struct fat32_dir_entry dirs[16];
    if (current_fs.dev->read_blocks(current_fs.dev, &dirs, lba, 1) != 0)
        return;
    for (int i = 0; i < 16; i++)
    {
        if (dirs[i].name[0] == 0x00)
            break;
        if (name_match(filename, &dirs[i]))
        {
            uint32_t cluster = (dirs[i].cluster_high << 16) | dirs[i].cluster_low;
            uint32_t size = dirs[i].size;
            pr_info("fat32: reading %s (%u bytes)...\n", filename, size);
            char buffer[512];
            while (cluster < 0x0FFFFFF8)
            {
                uint32_t file_lba = cluster_to_lba(cluster);
                for (int j = 0; j < (int)current_fs.sectors_per_cluster; j++)
                {
                    if (current_fs.dev->read_blocks(current_fs.dev, &buffer, file_lba + j, 1) != 0)
                        break;
                    for (size_t buf_idx = 0; buf_idx < 512; buf_idx++)
                        printk("%c", buffer[buf_idx]);
                }
                cluster = get_next_cluster(cluster);
            }
            printk("\n");
            return;
        }
    }
    pr_info("fat32: file %s not found.\n", filename);
}

int fat32_init(const char* device_name)
{
    struct block_device* dev = block_device_lookup(device_name);
    if (!dev)
        return -PERS_ERR_NOT_FOUND;
    uint8_t sector0[512];
    if (dev->read_blocks(dev, sector0, 0, 1) != 0)
        return -PERS_ERR_IO_ERROR;
    uint16_t sig = *(uint16_t*)(sector0 + 510);
    if (sig != 0xAA55)
        return -PERS_ERR_INVALID_ARGUMENT;
    if (sector0[0] == 0xEB || sector0[0] == 0xE9)
    {
        current_fs.dev = dev;
        current_fs.partition_lba_start = 0;
    }
    else
    {
        struct mbr* mbr = (struct mbr*)sector0;
        int found = 0;
        for (size_t i = 0; i < 4 && !found; i++)
        {
            if (mbr->partitions[i].type == 0x0B || mbr->partitions[i].type == 0x0C)
            {
                current_fs.dev = dev;
                current_fs.partition_lba_start = mbr->partitions[i].lba_start;
                found = 1;
            }
        }
        if (!found)
            return -PERS_ERR_NOT_FOUND;
    }
    struct fat32_bpb bpb;
    if (dev->read_blocks(dev, &bpb, current_fs.partition_lba_start, 1) != 0)
        return -PERS_ERR_IO_ERROR;
    current_fs.bytes_per_sector = bpb.bytes_per_sector;
    current_fs.reserved_sectors = bpb.reserved_sectors;
    current_fs.sectors_per_cluster = bpb.sectors_per_cluster;
    current_fs.root_cluster = bpb.root_cluster;
    current_fs.sectors_per_fat = bpb.sectors_per_fat_32;
    current_fs.num_fats = bpb.num_fats;
    current_fs.fat_lba_start = current_fs.partition_lba_start + current_fs.reserved_sectors;
    current_fs.data_lba_start = current_fs.fat_lba_start + (current_fs.num_fats * current_fs.sectors_per_fat);
    pr_info("fat32: partition at LBA %u\n", current_fs.partition_lba_start);
    return PERS_SUCCESS;
}
